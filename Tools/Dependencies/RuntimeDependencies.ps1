# Validate dynamically loaded vendor runtimes independently of PE imports.
function Test-HENativeRuntimeDependencies([string]$Root, [switch]$Shipping) {
    $rootPath = [IO.Path]::GetFullPath($Root)
    $contract = Get-Content -LiteralPath "$rootPath/HEngine.Runtime.json" -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json
    if ($contract.schema -ne 1) { throw 'Unsupported native runtime dependency contract.' }
    foreach ($flag in @('fsr','dx12','vulkan','oidn')) {
        if ($contract.$flag -isnot [bool]) { throw "Invalid runtime dependency flag: $flag" }
    }
    if ($Shipping -and $contract.oidn) { throw 'OIDN/offline renderer must not enter Shipping.' }
    $architectureProperty = $contract.PSObject.Properties['architecture']
    $architecture = if ($architectureProperty) { [string]$architectureProperty.Value } else { 'x64' }
    if ($architecture -notin @('x64','arm64')) { throw 'Unsupported native runtime architecture.' }
    if ($architecture -eq 'arm64' -and $contract.oidn) { throw 'Windows ARM64 does not ship the OIDN backend.' }
    $lockName = if ($architecture -eq 'arm64') { 'native-runtime.arm64.lock.json' } else { 'native-runtime.lock.json' }
    $lock = Get-Content -LiteralPath "$PSScriptRoot/$lockName" -Raw -Encoding UTF8 | ConvertFrom-Json
    $required = [Collections.Generic.List[string]]::new()
    $licenses = [Collections.Generic.List[string]]::new()
    if ($contract.dx12) { $required.Add('D3D12/D3D12Core.dll') }
    foreach ($backend in @('dx12','vk')) {
        $graphicsEnabled = if ($backend -eq 'dx12') { $contract.dx12 } else { $contract.vulkan }
        $property = $contract.PSObject.Properties[$(if ($backend -eq 'dx12') { 'fsrDx12' } else { 'fsrVulkan' })]
        if ($architecture -eq 'arm64' -and $contract.fsr -and !$property) { throw 'ARM64 FSR requires explicit backend capabilities.' }
        $enabled = if ($property) { $property.Value } else { $contract.fsr -and $graphicsEnabled }
        if ($enabled -isnot [bool] -or ($enabled -and (!$contract.fsr -or !$graphicsEnabled))) { throw 'Invalid FSR backend capability.' }
        if ($architecture -eq 'arm64' -and $backend -eq 'vk' -and $enabled) { throw 'The pinned FSR SDK has no Vulkan ARM64 runtime.' }
        $relative = "amd_fidelityfx_$backend.dll"
        if ($enabled) { $required.Add($relative) }
        elseif (Test-Path -LiteralPath "$rootPath/$relative") { throw "Disabled FSR backend left a runtime: $relative" }
    }
    if ($contract.fsr) { $licenses.Add('Licenses/FidelityFX.txt') }
    if ($contract.oidn) {
        foreach ($file in @('OpenImageDenoise.dll','OpenImageDenoise_core.dll','OpenImageDenoise_device_cpu.dll')) {
            $required.Add("Plugins/OfflineReferenceRenderer/$file")
        }
        $required.Add('tbb12.dll')
        foreach ($file in @('LICENSE.txt','third-party-programs.txt','third-party-programs-oneTBB.txt')) {
            $licenses.Add("licenses/OfflineReferenceRenderer/$file")
        }
        foreach ($file in Get-ChildItem -LiteralPath "$rootPath/Plugins/OfflineReferenceRenderer" -Filter '*.dll' -File -ErrorAction Stop) {
            if (!$required.Contains("Plugins/OfflineReferenceRenderer/$($file.Name)")) { throw "Unexpected OIDN backend or conflicting shared runtime: $($file.Name)" }
        }
    } elseif (Test-Path -LiteralPath "$rootPath/Plugins/OfflineReferenceRenderer") {
        throw 'Disabled offline renderer left plugin runtime files.'
    }
    if (@(Get-ChildItem -LiteralPath $rootPath -Filter 'OpenImageDenoise*.dll' -File).Count) {
        throw 'OIDN runtime must belong to Plugins/OfflineReferenceRenderer, not the executable root.'
    }
    $dynamicRoots = @()
    foreach ($relative in $required) {
        $path = Join-Path $rootPath $relative
        if ($architecture -eq 'arm64' -and $relative -eq 'amd_fidelityfx_dx12.dll') {
            $sourcePin = $contract.PSObject.Properties['fsrSourceArchiveSha256']
            $runtimeHash = $contract.PSObject.Properties['fsrRuntimeSha256']
            if (!$sourcePin -or $sourcePin.Value -cne $lock.fsrSourceArchiveSha256 -or
                !$runtimeHash -or $runtimeHash.Value -cnotmatch '^[0-9a-f]{64}$') { throw 'Missing or invalid ARM64 FSR build provenance.' }
            $expected = $runtimeHash.Value
        } else { $expected = $lock.files.$relative }
        if (!$expected -or !(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing native runtime: $relative" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected) { throw "Native runtime checksum mismatch: $relative" }
        $dynamicRoots += $path
    }
    foreach ($relative in $licenses) {
        if (!(Test-Path -LiteralPath "$rootPath/$relative" -PathType Leaf) -or (Get-Item -LiteralPath "$rootPath/$relative").Length -eq 0) { throw "Missing native runtime license: $relative" }
    }
    if ($contract.fsr -or $contract.oidn) {
        foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
            $path = Join-Path $rootPath $name
            if ($architecture -eq 'arm64' -and $name -eq 'vcruntime140_1.dll' -and !(Test-Path -LiteralPath $path)) { continue }
            if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing app-local vendor CRT: $name" }
            $dynamicRoots += $path
        }
        if (!(Test-Path -LiteralPath "$rootPath/Licenses/Microsoft-Visual-Cpp-Runtime.txt" -PathType Leaf)) {
            throw 'Missing vendor CRT redistribution notice.'
        }
    }
    [pscustomobject]@{Root=$rootPath;Fsr=$contract.fsr;Oidn=$contract.oidn;DynamicRoots=$dynamicRoots;Checked=$required.Count}
}
