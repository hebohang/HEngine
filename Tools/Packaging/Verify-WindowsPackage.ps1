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
    [switch]$RequireTargetGraphAndLinkMap
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-ByteRange {
    param(
        [byte[]]$Bytes,
        [long]$Offset,
        [long]$Count,
        [string]$Description
    )

    if ($Offset -lt 0 -or $Count -lt 0 -or
        $Offset -gt $Bytes.LongLength -or
        $Count -gt ($Bytes.LongLength - $Offset)) {
        throw "Invalid PE $Description range: offset=$Offset count=$Count size=$($Bytes.LongLength)"
    }
}

function Read-UInt16 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 2 -Description 'UInt16'
    return [BitConverter]::ToUInt16($Bytes, [int]$Offset)
}

function Read-UInt32 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 4 -Description 'UInt32'
    return [BitConverter]::ToUInt32($Bytes, [int]$Offset)
}

function Read-UInt64 {
    param([byte[]]$Bytes, [long]$Offset)
    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 8 -Description 'UInt64'
    return [BitConverter]::ToUInt64($Bytes, [int]$Offset)
}

function Read-AsciiString {
    param(
        [byte[]]$Bytes,
        [long]$Offset,
        [int]$MaximumLength = 4096
    )

    Assert-ByteRange -Bytes $Bytes -Offset $Offset -Count 1 -Description 'string'
    $characters = New-Object System.Collections.Generic.List[byte]
    for ($index = 0; $index -lt $MaximumLength; ++$index) {
        Assert-ByteRange -Bytes $Bytes -Offset ($Offset + $index) -Count 1 -Description 'string'
        $value = $Bytes[$Offset + $index]
        if ($value -eq 0) {
            return [Text.Encoding]::ASCII.GetString($characters.ToArray())
        }
        [void]$characters.Add($value)
    }
    throw "Unterminated PE string at file offset $Offset"
}

function Get-PeImage {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.LongLength -lt 64 -or
        $bytes[0] -ne [byte][char]'M' -or
        $bytes[1] -ne [byte][char]'Z') {
        throw "Not a PE image: $Path"
    }

    $peOffset = [long](Read-UInt32 -Bytes $bytes -Offset 0x3c)
    if ((Read-UInt32 -Bytes $bytes -Offset $peOffset) -ne 0x00004550) {
        throw "Invalid PE signature: $Path"
    }

    $coffOffset = $peOffset + 4
    $machine = Read-UInt16 -Bytes $bytes -Offset $coffOffset
    $sectionCount = Read-UInt16 -Bytes $bytes -Offset ($coffOffset + 2)
    $optionalHeaderSize = Read-UInt16 -Bytes $bytes -Offset ($coffOffset + 16)
    $optionalOffset = $coffOffset + 20
    $optionalMagic = Read-UInt16 -Bytes $bytes -Offset $optionalOffset
    if ($optionalMagic -eq 0x20b) {
        $imageBase = Read-UInt64 -Bytes $bytes -Offset ($optionalOffset + 24)
        $directoryCountOffset = $optionalOffset + 108
        $directoryOffset = $optionalOffset + 112
    }
    elseif ($optionalMagic -eq 0x10b) {
        $imageBase = [uint64](Read-UInt32 -Bytes $bytes -Offset ($optionalOffset + 28))
        $directoryCountOffset = $optionalOffset + 92
        $directoryOffset = $optionalOffset + 96
    }
    else {
        throw ('Unsupported PE optional-header magic 0x{0:x}: {1}' -f $optionalMagic, $Path)
    }

    $directoryCount = Read-UInt32 -Bytes $bytes -Offset $directoryCountOffset
    $sizeOfHeaders = Read-UInt32 -Bytes $bytes -Offset ($optionalOffset + 60)
    $sectionTableOffset = $optionalOffset + $optionalHeaderSize
    $sections = New-Object System.Collections.Generic.List[object]
    for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; ++$sectionIndex) {
        $sectionOffset = $sectionTableOffset + ($sectionIndex * 40)
        Assert-ByteRange -Bytes $bytes -Offset $sectionOffset -Count 40 -Description 'section header'
        $nameBytes = $bytes[$sectionOffset..($sectionOffset + 7)]
        $nameLength = [Array]::IndexOf($nameBytes, [byte]0)
        if ($nameLength -lt 0) { $nameLength = 8 }
        $name = [Text.Encoding]::ASCII.GetString($nameBytes, 0, $nameLength)
        [void]$sections.Add([pscustomobject]@{
            Name = $name
            VirtualSize = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 8)
            VirtualAddress = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 12)
            RawSize = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 16)
            RawOffset = Read-UInt32 -Bytes $bytes -Offset ($sectionOffset + 20)
        })
    }

    return [pscustomobject]@{
        Path = $Path
        Bytes = $bytes
        Machine = $machine
        OptionalMagic = $optionalMagic
        ImageBase = $imageBase
        DirectoryCount = $directoryCount
        DirectoryOffset = $directoryOffset
        SizeOfHeaders = $sizeOfHeaders
        Sections = $sections.ToArray()
    }
}

function Convert-RvaToFileOffset {
    param(
        [object]$Image,
        [uint32]$Rva
    )

    if ($Rva -lt $Image.SizeOfHeaders) {
        Assert-ByteRange -Bytes $Image.Bytes -Offset $Rva -Count 1 -Description 'header RVA'
        return [long]$Rva
    }

    foreach ($section in $Image.Sections) {
        $mappedSize = [Math]::Max([uint64]$section.VirtualSize, [uint64]$section.RawSize)
        $start = [uint64]$section.VirtualAddress
        $end = $start + $mappedSize
        if ([uint64]$Rva -ge $start -and [uint64]$Rva -lt $end) {
            $offset = [uint64]$section.RawOffset + ([uint64]$Rva - $start)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $offset -Count 1 -Description 'section RVA'
            return [long]$offset
        }
    }
    throw ('RVA 0x{0:x} is outside the PE sections: {1}' -f $Rva, $Image.Path)
}

function Get-PeDataDirectory {
    param(
        [object]$Image,
        [int]$Index
    )

    if ($Index -lt 0 -or $Index -ge $Image.DirectoryCount) {
        return $null
    }
    $entryOffset = $Image.DirectoryOffset + ($Index * 8)
    $rva = Read-UInt32 -Bytes $Image.Bytes -Offset $entryOffset
    $size = Read-UInt32 -Bytes $Image.Bytes -Offset ($entryOffset + 4)
    if ($rva -eq 0 -or $size -eq 0) {
        return $null
    }
    return [pscustomobject]@{ Rva = $rva; Size = $size }
}

function Get-PeNormalImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $importDirectory = Get-PeDataDirectory -Image $Image -Index 1
    if ($null -ne $importDirectory) {
        $descriptorOffset = Convert-RvaToFileOffset -Image $Image -Rva $importDirectory.Rva
        for ($descriptorIndex = 0; $descriptorIndex -lt 4096; ++$descriptorIndex) {
            $currentOffset = $descriptorOffset + ($descriptorIndex * 20)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $currentOffset -Count 20 -Description 'import descriptor'
            $originalThunk = Read-UInt32 -Bytes $Image.Bytes -Offset $currentOffset
            $timeDateStamp = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 4)
            $forwarderChain = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 8)
            $nameRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 12)
            $firstThunk = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 16)
            if (($originalThunk -bor $timeDateStamp -bor $forwarderChain -bor $nameRva -bor $firstThunk) -eq 0) {
                break
            }
            if ($nameRva -eq 0) {
                throw "Import descriptor has no DLL name: $($Image.Path)"
            }
            $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameRva
            [void]$imports.Add((Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset))
        }
    }

    return @($imports | Sort-Object)
}

function Get-PeDelayImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $delayDirectory = Get-PeDataDirectory -Image $Image -Index 13
    if ($null -ne $delayDirectory) {
        $descriptorOffset = Convert-RvaToFileOffset -Image $Image -Rva $delayDirectory.Rva
        for ($descriptorIndex = 0; $descriptorIndex -lt 4096; ++$descriptorIndex) {
            $currentOffset = $descriptorOffset + ($descriptorIndex * 32)
            Assert-ByteRange -Bytes $Image.Bytes -Offset $currentOffset -Count 32 -Description 'delay import descriptor'
            $attributes = Read-UInt32 -Bytes $Image.Bytes -Offset $currentOffset
            $nameValue = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 4)
            $moduleHandle = Read-UInt32 -Bytes $Image.Bytes -Offset ($currentOffset + 8)
            if (($attributes -bor $nameValue -bor $moduleHandle) -eq 0) {
                break
            }
            if (($attributes -band 1) -eq 0) {
                throw "Legacy VA-based delay imports are not supported: $($Image.Path)"
            }
            $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameValue
            [void]$imports.Add((Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset))
        }
    }

    return @($imports | Sort-Object)
}

function Get-PeImports {
    param([object]$Image)

    $imports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @(Get-PeNormalImports -Image $Image)) {
        [void]$imports.Add($name)
    }
    foreach ($name in @(Get-PeDelayImports -Image $Image)) {
        [void]$imports.Add($name)
    }
    return @($imports | Sort-Object)
}

function Get-PeExports {
    param([object]$Image)

    $exports = @{}
    $exportDirectory = Get-PeDataDirectory -Image $Image -Index 0
    if ($null -eq $exportDirectory) {
        return $exports
    }

    $directoryOffset = Convert-RvaToFileOffset -Image $Image -Rva $exportDirectory.Rva
    Assert-ByteRange -Bytes $Image.Bytes -Offset $directoryOffset -Count 40 -Description 'export directory'
    $functionCount = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 20)
    $nameCount = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 24)
    $functionsRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 28)
    $namesRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 32)
    $ordinalsRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($directoryOffset + 36)
    if ($nameCount -gt $functionCount -or $nameCount -gt 100000) {
        throw "Invalid export counts: $($Image.Path)"
    }

    $functionsOffset = Convert-RvaToFileOffset -Image $Image -Rva $functionsRva
    $namesOffset = Convert-RvaToFileOffset -Image $Image -Rva $namesRva
    $ordinalsOffset = Convert-RvaToFileOffset -Image $Image -Rva $ordinalsRva
    for ($nameIndex = 0; $nameIndex -lt $nameCount; ++$nameIndex) {
        $nameRva = Read-UInt32 -Bytes $Image.Bytes -Offset ($namesOffset + ($nameIndex * 4))
        $nameOffset = Convert-RvaToFileOffset -Image $Image -Rva $nameRva
        $name = Read-AsciiString -Bytes $Image.Bytes -Offset $nameOffset
        $ordinalIndex = Read-UInt16 -Bytes $Image.Bytes -Offset ($ordinalsOffset + ($nameIndex * 2))
        if ($ordinalIndex -ge $functionCount) {
            throw "Invalid export ordinal for ${name}: $($Image.Path)"
        }
        $exports[$name] = Read-UInt32 -Bytes $Image.Bytes -Offset ($functionsOffset + ($ordinalIndex * 4))
    }
    return $exports
}

function Read-ExportedUInt32 {
    param([object]$Image, [hashtable]$Exports, [string]$Name)
    if (-not $Exports.ContainsKey($Name)) {
        throw "Required data export '$Name' is missing: $($Image.Path)"
    }
    $offset = Convert-RvaToFileOffset -Image $Image -Rva $Exports[$Name]
    return Read-UInt32 -Bytes $Image.Bytes -Offset $offset
}

function Read-ExportedStringPointer {
    param([object]$Image, [hashtable]$Exports, [string]$Name)
    if (-not $Exports.ContainsKey($Name)) {
        throw "Required data export '$Name' is missing: $($Image.Path)"
    }
    if ($Image.OptionalMagic -ne 0x20b) {
        throw "The Agility SDK export verifier requires a 64-bit PE image: $($Image.Path)"
    }
    $exportOffset = Convert-RvaToFileOffset -Image $Image -Rva $Exports[$Name]
    $pointerValue = Read-UInt64 -Bytes $Image.Bytes -Offset $exportOffset
    if ($pointerValue -ge $Image.ImageBase) {
        $stringRva64 = $pointerValue - $Image.ImageBase
    }
    else {
        # Accept an RVA-form pointer for toolchains that emit one before fixups.
        $stringRva64 = $pointerValue
    }
    if ($stringRva64 -gt [uint32]::MaxValue) {
        throw "Export '$Name' points outside the PE image: $($Image.Path)"
    }
    $stringOffset = Convert-RvaToFileOffset -Image $Image -Rva ([uint32]$stringRva64)
    return Read-AsciiString -Bytes $Image.Bytes -Offset $stringOffset
}

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
foreach ($binary in $binaryFiles) {
    if ($binaryByName.ContainsKey($binary.Name)) {
        throw "Package contains conflicting PE basenames: $($binary.Name)"
    }
    $binaryByName[$binary.Name] = $binary.FullName
    $image = Get-PeImage -Path $binary.FullName
    if ($image.Machine -ne 0x8664 -or $image.OptionalMagic -ne 0x20b) {
        throw ('Package binary is not Windows x64 (machine=0x{0:x}, magic=0x{1:x}): {2}' -f
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
$dependencyRows = New-Object System.Collections.Generic.List[object]

while ($queue.Count -gt 0) {
    $binaryPath = $queue.Dequeue()
    if (-not $visited.Add($binaryPath)) {
        continue
    }
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
if ($null -eq $vcRuntime) {
    throw 'python311.dll requires app-local vcruntime140.dll, but it is missing.'
}
$pythonImage = $imagesByPath[$binaryByName['python311.dll']]
if ($null -eq $pythonImage -or
    -not (@(Get-PeImports -Image $pythonImage) -contains 'VCRUNTIME140.dll')) {
    throw 'Packaged Python no longer imports vcruntime140.dll; remove the now-unnecessary CRT file.'
}

$dependencyRows |
    Sort-Object Binary, Import |
    Format-Table -AutoSize | Out-String | Write-Host
$vcVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($vcRuntime.FullName).FileVersion
Write-Host "Verified Windows x64 recursive PE dependency closure ($($visited.Count) app-local binaries)."
Write-Host "Verified /MT executable; dynamic CRT is required only by python311.dll (vcruntime140.dll $vcVersion)."
Write-Host "Verified DirectX 12 Agility SDK $ExpectedAgilityPackageVersion (SDK $mainSdkVersion) at .\D3D12\; debug layers are absent."
Write-Host 'Verified dual-backend Vulkan policy: vulkan-1.dll is delay-loaded from the installed graphics driver and is not packaged app-local.'
