param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$ExecutableName,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$ExpectedD3D12SDKVersion,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+[.][0-9]+[.][0-9]+$')]
    [string]$ExpectedAgilityPackageVersion,

    [ValidateSet('x64','arm64')][string]$Architecture = 'x64',

    [string[]]$ForbiddenPolicyTokens = @(
        'OfflineReferenceRenderer',
        'he.offline-reference-renderer',
        'ReferencePathTracer',
        'HardwareRayTracingReference',
        'PackageNever'
    ),

    [string[]]$ForbiddenPolicyContentPrefixes = @(
        'Assets/DevOnly',
        'Config/DevOnly',
        'Config/Plugins.json',
        'Config/UserEngine.json',
        'Saved/Config/UserEngine.json',
        'Scripts/DevOnly',
        'Shaders/DevOnly'
    ),

    [string]$TargetGraphPath = '',
    [string]$LinkMapPath = '',
    [string]$PdbPath = '',
    [string]$ShaderManifestPath = '',
    [string]$ShaderCacheDirectory = '',
    [switch]$RequireTargetGraphAndLinkMap,
    [ValidateSet('enabled','disabled')][string]$PythonPlugin = 'enabled',
    [ValidateSet('enabled','disabled')][string]$CSharpPlugin = 'disabled'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'PeImage.ps1')
$expectedMachine = if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }

$packagePath = [IO.Path]::GetFullPath($PackageDir).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $packagePath -PathType Container)) {
    throw "Package directory does not exist: $packagePath"
}

$mainExecutable = Join-Path $packagePath ($ExecutableName + '.exe')
$agilityCore = Join-Path $packagePath 'D3D12\D3D12Core.dll'
foreach ($requiredBinary in @($mainExecutable, $agilityCore)) {
    if (-not (Test-Path -LiteralPath $requiredBinary -PathType Leaf)) {
        throw "Required package binary is missing: $requiredBinary"
    }
}

$policyVerifier = Join-Path $PSScriptRoot 'Verify-PackagePolicy.ps1'
if (-not (Test-Path -LiteralPath $policyVerifier -PathType Leaf)) {
    throw "Package policy verifier is missing: $policyVerifier"
}
$policyArguments = @{
    ForbiddenTokens = $ForbiddenPolicyTokens
    ForbiddenContentPrefixes = $ForbiddenPolicyContentPrefixes
    PackageDirectories = @($packagePath)
}
if (-not [string]::IsNullOrWhiteSpace($TargetGraphPath)) {
    $policyArguments.TargetGraphPath = $TargetGraphPath
}
if (-not [string]::IsNullOrWhiteSpace($LinkMapPath)) {
    $policyArguments.LinkMapPath = $LinkMapPath
}
if (-not [string]::IsNullOrWhiteSpace($PdbPath)) {
    $policyArguments.PdbPaths = @($PdbPath)
}
if (-not [string]::IsNullOrWhiteSpace($ShaderManifestPath)) {
    $policyArguments.ShaderManifestPaths = @($ShaderManifestPath)
}
if (-not [string]::IsNullOrWhiteSpace($ShaderCacheDirectory)) {
    $policyArguments.ShaderCacheDirectories = @($ShaderCacheDirectory)
}
if ($RequireTargetGraphAndLinkMap) {
    $policyArguments.RequireTargetGraphAndLinkMap = $true
}
& $policyVerifier @policyArguments

$debugLayers = @(Get-ChildItem -LiteralPath $packagePath -Recurse -File |
    Where-Object { $_.Name -ieq 'D3D12SDKLayers.dll' })
if ($debugLayers.Count -ne 0) {
    throw "Release package must not contain D3D12SDKLayers.dll: $($debugLayers[0].FullName)"
}
$appLocalVulkanLoaders = @(Get-ChildItem -LiteralPath $packagePath -Recurse -File |
    Where-Object { $_.Name -ieq 'vulkan-1.dll' })
if ($appLocalVulkanLoaders.Count -ne 0) {
    throw "The package must use the graphics-driver Vulkan loader, not an app-local vulkan-1.dll: $($appLocalVulkanLoaders[0].FullName)"
}

$binaryFiles = @(Get-ChildItem -LiteralPath $packagePath -Recurse -File |
    Where-Object { $_.Extension -ieq '.exe' -or $_.Extension -ieq '.dll' })
$binaryByName = @{}
$imagesByPath = @{}
$scriptFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$managedOnly = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
if ($CSharpPlugin -eq 'enabled') {
    $verifiedScripts = & (Join-Path $PSScriptRoot '../Scripting/VerifyPackageRuntime.ps1') -Directory (Join-Path $packagePath 'Scripting/CSharp')
    foreach ($file in $verifiedScripts.Files) { [void]$scriptFiles.Add($file) }
} elseif (Test-Path -LiteralPath (Join-Path $packagePath 'Scripting')) { throw 'Disabled C# plugin left a scripting runtime in the package.' }
if ($PythonPlugin -eq 'disabled' -and (Test-Path -LiteralPath (Join-Path $packagePath 'python311.dll'))) { throw 'Disabled Python plugin left its runtime in the package.' }
foreach ($binary in $binaryFiles) {
    if ($binaryByName.ContainsKey($binary.Name)) {
        throw "Package contains conflicting PE basenames: $($binary.Name)"
    }
    $binaryByName[$binary.Name] = $binary.FullName
    $image = Get-PeImage -Path $binary.FullName
    $bManagedAnyCpu = $false
    if ($scriptFiles.Contains($binary.FullName) -and $image.Machine -eq 0x14c -and $image.OptionalMagic -eq 0x10b) {
        $clr = Get-PeDataDirectory -Image $image -Index 14
        if ($null -ne $clr) {
            $clrOffset = Convert-RvaToFileOffset -Image $image -Rva $clr.Rva
            $flags = Read-UInt32 -Bytes $image.Bytes -Offset ($clrOffset + 16)
            $bManagedAnyCpu = ($flags -band 1) -and !($flags -band 0x20012)
        }
    }
    if ($bManagedAnyCpu) { [void]$managedOnly.Add($binary.FullName) }
    elseif ($image.Machine -ne $expectedMachine -or $image.OptionalMagic -ne 0x20b) {
        throw ("Package binary is not Windows $Architecture (machine=0x{0:x}, magic=0x{1:x}): {2}" -f
            $image.Machine, $image.OptionalMagic, $binary.FullName)
    }
    $imagesByPath[$binary.FullName] = $image
}

$mainImage = $imagesByPath[$mainExecutable]
$mainExports = Get-PeExports -Image $mainImage
$mainSdkVersion = Read-ExportedUInt32 -Image $mainImage -Exports $mainExports -Name 'D3D12SDKVersion'
$mainSdkPath = Read-ExportedStringPointer -Image $mainImage -Exports $mainExports -Name 'D3D12SDKPath'
if ($mainSdkVersion -ne $ExpectedD3D12SDKVersion) {
    throw "Main executable exports D3D12SDKVersion=$mainSdkVersion; expected $ExpectedD3D12SDKVersion"
}
if ($mainSdkPath -cne '.\D3D12\') {
    throw "Main executable exports an invalid D3D12SDKPath: '$mainSdkPath'"
}

$coreImage = $imagesByPath[$agilityCore]
$coreExports = Get-PeExports -Image $coreImage
$coreSdkVersion = Read-ExportedUInt32 -Image $coreImage -Exports $coreExports -Name 'D3D12SDKVersion'
if ($coreSdkVersion -ne $mainSdkVersion) {
    throw "D3D12Core.dll SDK version $coreSdkVersion does not match the executable export $mainSdkVersion"
}
if (-not $coreExports.ContainsKey('D3D12GetInterface')) {
    throw "Packaged D3D12Core.dll does not export D3D12GetInterface"
}
$coreFileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($agilityCore).FileVersion
$expectedCoreVersionPrefix = [Regex]::Escape($ExpectedAgilityPackageVersion) + '(?:[.]|$)'
if ($coreFileVersion -notmatch ('^' + $expectedCoreVersionPrefix)) {
    throw "D3D12Core.dll file version '$coreFileVersion' is not package $ExpectedAgilityPackageVersion"
}

$mainImports = @(Get-PeImports -Image $mainImage)
$mainNormalImports = @(Get-PeNormalImports -Image $mainImage)
$mainDelayImports = @(Get-PeDelayImports -Image $mainImage)
if ($mainNormalImports -contains 'vulkan-1.dll') {
    throw 'vulkan-1.dll must be delay-loaded so the default DX12 path also starts on systems without a Vulkan loader.'
}
if (-not ($mainDelayImports -contains 'vulkan-1.dll')) {
    throw 'The dual-backend executable does not delay-import vulkan-1.dll.'
}
$dynamicCrtPattern = '^(?:vcruntime|msvcp|concrt)[0-9].*[.]dll$'
foreach ($importName in $mainImports) {
    if ($importName -match $dynamicCrtPattern) {
        throw "The /MT game executable unexpectedly imports dynamic CRT library: $importName"
    }
}

$systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
if ([string]::IsNullOrWhiteSpace($systemDirectory) -or
    -not (Test-Path -LiteralPath $systemDirectory -PathType Container)) {
    throw 'Could not resolve the native Windows System32 directory.'
}

$mustBeAppLocalPattern =
    '^(?:(?:vcruntime|msvcp|concrt)[0-9].*|python[0-9].*|tbb.*|embree.*|d3d12core|d3d12sdklayers)[.]dll$'
$visited = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$queue = New-Object 'System.Collections.Generic.Queue[string]'
$queue.Enqueue($mainExecutable)
$queue.Enqueue($agilityCore)
. (Join-Path $PSScriptRoot '../Dependencies/RuntimeDependencies.ps1')
$nativeDependencies = Test-HENativeRuntimeDependencies -Root $packagePath -Shipping
foreach ($binary in $nativeDependencies.DynamicRoots) { $queue.Enqueue($binary) }
# CLR loads assemblies, hostpolicy and JIT through runtime metadata. Only files
# authenticated against the pinned runtime/build receipt may be dynamic roots.
foreach ($binary in $binaryFiles) { if ($scriptFiles.Contains($binary.FullName)) { $queue.Enqueue($binary.FullName) } }
$dependencyRows = New-Object System.Collections.Generic.List[object]

while ($queue.Count -gt 0) {
    $binaryPath = $queue.Dequeue()
    if (-not $visited.Add($binaryPath)) {
        continue
    }
    # The legacy mscoree import in an IL-only AnyCPU image is not the native
    # loading path used by CoreCLR. Mixed-mode or unlisted x86 images were rejected.
    if ($managedOnly.Contains($binaryPath)) { continue }
    $image = $imagesByPath[$binaryPath]
    $imports = @()
    $imports += @(Get-PeNormalImports -Image $image | ForEach-Object {
        [pscustomobject]@{ Name = $_; Kind = 'normal' }
    })
    $imports += @(Get-PeDelayImports -Image $image | ForEach-Object {
        [pscustomobject]@{ Name = $_; Kind = 'delay' }
    })
    foreach ($import in $imports) {
        $importName = $import.Name
        $resolution = ''
        if ($binaryByName.ContainsKey($importName)) {
            $resolvedPath = $binaryByName[$importName]
            $resolution = 'app-local'
            if (-not $visited.Contains($resolvedPath)) {
                $queue.Enqueue($resolvedPath)
            }
        }
        elseif ($importName -match '^(?:api-ms-win-|ext-ms-win-).+[.]dll$') {
            $resolvedPath = '<Windows API set>'
            $resolution = 'Windows API set'
        }
        elseif ($importName -match $mustBeAppLocalPattern) {
            throw "Missing app-local runtime dependency '$importName' imported by '$binaryPath'"
        }
        elseif ($import.Kind -eq 'delay' -and $importName -ieq 'vulkan-1.dll') {
            $resolvedPath = '<graphics-driver Vulkan loader>'
            $resolution = 'Windows graphics runtime (delay-load)'
        }
        else {
            $resolvedPath = Join-Path $systemDirectory $importName
            if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
                throw "Unresolved PE dependency '$importName' imported by '$binaryPath'"
            }
            $resolution = 'Windows system'
        }
        [void]$dependencyRows.Add([pscustomobject]@{
            Binary = $binaryPath.Substring($packagePath.Length).TrimStart('\', '/')
            Import = $importName
            Kind = $import.Kind
            Resolution = $resolution
        })
    }
}

foreach ($binary in $binaryFiles) {
    if (-not $visited.Contains($binary.FullName)) {
        throw "Package contains an unreferenced or unexpected app-local binary: $($binary.FullName)"
    }
}

$vcRuntime = $binaryFiles | Where-Object { $_.Name -ieq 'vcruntime140.dll' } |
    Select-Object -First 1
if ($PythonPlugin -eq 'enabled' -and $null -eq $vcRuntime) {
    throw 'python311.dll requires app-local vcruntime140.dll, but it is missing.'
}
if ($PythonPlugin -eq 'enabled') {
    $pythonImage = $imagesByPath[$binaryByName['python311.dll']]
    if ($null -eq $pythonImage -or -not (@(Get-PeImports -Image $pythonImage) -contains 'VCRUNTIME140.dll')) {
        throw 'Packaged Python no longer imports vcruntime140.dll; remove the now-unnecessary CRT file.'
    }
}

$dependencyRows |
    Sort-Object Binary, Import |
    Format-Table -AutoSize | Out-String | Write-Host
Write-Host "Verified Windows x64 recursive PE dependency closure ($($visited.Count) app-local binaries)."
Write-Host "Verified /MT executable; Python=$PythonPlugin, C#=$CSharpPlugin, IL-only assemblies=$($managedOnly.Count)."
Write-Host "Verified DirectX 12 Agility SDK $ExpectedAgilityPackageVersion (SDK $mainSdkVersion) at .\D3D12\; debug layers are absent."
Write-Host 'Verified dual-backend Vulkan policy: vulkan-1.dll is delay-loaded from the installed graphics driver and is not packaged app-local.'
