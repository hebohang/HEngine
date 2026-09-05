param(
    [string[]]$ForbiddenTokens = @(
        'OfflineReferenceRenderer',
        'he.offline-reference-renderer',
        'ReferencePathTracer',
        'HardwareRayTracingReference',
        'PackageNever'
    ),

    [string[]]$ForbiddenContentPrefixes = @(
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
    [string[]]$ExecutablePaths = @(),
    [string[]]$PdbPaths = @(),
    [string[]]$MapPaths = @(),
    [string[]]$ShaderManifestPaths = @(),
    [string[]]$ShaderCacheDirectories = @(),
    [string[]]$GameManifestPaths = @(),
    [string[]]$GamePakPaths = @(),
    [string[]]$PackageDirectories = @(),
    [string[]]$ExtractedRoots = @(),
    [switch]$RequireBuildGraphInput,
    [switch]$RequireTargetGraphAndLinkMap,
    [switch]$Contract
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Latin1 = [Text.Encoding]::GetEncoding(28591)
$script:ForbiddenTokens = @($ForbiddenTokens | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
})
$script:ForbiddenPrefixes = @($ForbiddenContentPrefixes | ForEach-Object {
    $_.Replace('\', '/').Trim('/')
} | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$script:EncodedTokenPatterns = New-Object System.Collections.Generic.List[object]
$script:MaximumPatternLength = 1
$script:SuppressSuccess = $false

if ($script:ForbiddenTokens.Count -eq 0) {
    throw 'Package policy requires at least one forbidden token.'
}

foreach ($token in $script:ForbiddenTokens) {
    foreach ($bytes in @(
        [Text.Encoding]::ASCII.GetBytes($token),
        [Text.Encoding]::Unicode.GetBytes($token),
        [Text.Encoding]::BigEndianUnicode.GetBytes($token))) {
        $pattern = $script:Latin1.GetString($bytes)
        [void]$script:EncodedTokenPatterns.Add([pscustomobject]@{
            Token = $token
            Pattern = $pattern
        })
        $script:MaximumPatternLength = [Math]::Max(
            $script:MaximumPatternLength,
            $pattern.Length)
    }
}

function Assert-RegularPolicyFile {
    param(
        [string]$Path,
        [string]$Description,
        [switch]$AllowEmpty
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description path is empty."
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must be a regular file: $($item.FullName)"
    }
    if (-not $AllowEmpty -and $item.Length -eq 0) {
        throw "$Description must not be empty: $($item.FullName)"
    }
    return $item.FullName
}

function Assert-RegularPolicyDirectory {
    param([string]$Path, [string]$Description)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description path is empty."
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must be a regular directory: $($item.FullName)"
    }
    return $item.FullName.TrimEnd('\', '/')
}

function Assert-PolicyValue {
    param([string]$Value, [string]$Context)

    if ($null -eq $Value) {
        return
    }
    foreach ($token in $script:ForbiddenTokens) {
        if ($Value.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "$Context contains forbidden package token '$token'."
        }
    }
}

function Assert-PolicyPath {
    param([string]$Value, [string]$Context)

    Assert-PolicyValue -Value $Value -Context $Context
    $normalized = $Value.Replace('\', '/').Trim()
    foreach ($prefix in $script:ForbiddenPrefixes) {
        $comparison = [StringComparison]::OrdinalIgnoreCase
        if ($normalized.Equals($prefix, $comparison) -or
            $normalized.StartsWith($prefix + '/', $comparison) -or
            $normalized.IndexOf('/' + $prefix + '/', $comparison) -ge 0 -or
            $normalized.EndsWith('/' + $prefix, $comparison)) {
            throw "$Context enters forbidden developer-only content prefix '$prefix'."
        }
    }
}

function Assert-PolicyStream {
    param([IO.Stream]$Stream, [string]$Context)

    if (-not $Stream.CanRead) {
        throw "Package policy cannot read $Context."
    }
    $buffer = New-Object byte[] (1024 * 1024)
    $carry = ''
    while (($read = $Stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $chunk = $script:Latin1.GetString($buffer, 0, $read)
        $candidate = $carry + $chunk
        foreach ($encoded in $script:EncodedTokenPatterns) {
            if ($candidate.IndexOf(
                    $encoded.Pattern,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "$Context contains forbidden package token '$($encoded.Token)'."
            }
        }
        $carryLength = [Math]::Min(
            [Math]::Max($script:MaximumPatternLength - 1, 0),
            $candidate.Length)
        $carry = if ($carryLength -gt 0) {
            $candidate.Substring($candidate.Length - $carryLength)
        }
        else {
            ''
        }
    }
}

function Assert-PolicyFileContent {
    param(
        [string]$Path,
        [string]$Description,
        [switch]$AllowEmpty
    )

    $resolved = Assert-RegularPolicyFile `
        -Path $Path `
        -Description $Description `
        -AllowEmpty:$AllowEmpty
    $stream = [IO.File]::Open(
        $resolved,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        Assert-PolicyStream -Stream $stream -Context $Description
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-TargetGraphPolicy {
    param([string]$Path)

    $resolved = Assert-RegularPolicyFile `
        -Path $Path `
        -Description 'CMake package target graph'
    $graphText = [IO.File]::ReadAllText($resolved)
    if ($graphText.IndexOf('digraph', [StringComparison]::Ordinal) -lt 0 -or
        $graphText.IndexOf('HGame', [StringComparison]::Ordinal) -lt 0 -or
        $graphText.IndexOf('HEngineRuntime', [StringComparison]::Ordinal) -lt 0) {
        throw 'CMake package target graph is missing its HGame -> HEngineRuntime closure.'
    }
    Assert-PolicyFileContent `
        -Path $resolved `
        -Description 'CMake package target graph'
}

function Assert-LinkMapPolicy {
    param([string]$Path)

    $resolved = Assert-RegularPolicyFile `
        -Path $Path `
        -Description 'shipping link map'
    $mapText = [IO.File]::ReadAllText($resolved)
    if ($mapText.IndexOf('HEngineRuntime', [StringComparison]::OrdinalIgnoreCase) -lt 0 -or
        $mapText.IndexOf('.obj', [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw 'Shipping link map is missing HEngineRuntime static-library/object provenance.'
    }
    Assert-PolicyFileContent -Path $resolved -Description 'shipping link map'
}

function Assert-JsonNodePolicy {
    param([object]$Node, [string]$Context)

    if ($null -eq $Node) {
        return
    }
    if ($Node -is [string]) {
        Assert-PolicyPath -Value ([string]$Node) -Context $Context
        return
    }
    if ($Node -is [Collections.IDictionary]) {
        foreach ($key in $Node.Keys) {
            Assert-PolicyValue -Value ([string]$key) -Context "$Context property"
            Assert-JsonNodePolicy -Node $Node[$key] -Context "$Context.$key"
        }
        return
    }
    if ($Node -is [Collections.IEnumerable] -and -not ($Node -is [string])) {
        $index = 0
        foreach ($child in $Node) {
            Assert-JsonNodePolicy -Node $child -Context "$Context[$index]"
            ++$index
        }
        return
    }
    if ($Node -is [psobject]) {
        foreach ($property in $Node.PSObject.Properties) {
            Assert-PolicyValue -Value $property.Name -Context "$Context property"
            Assert-JsonNodePolicy `
                -Node $property.Value `
                -Context "$Context.$($property.Name)"
        }
    }
}

function Read-StructuredPolicyJson {
    param([string]$Path, [string]$Description)

    $resolved = Assert-RegularPolicyFile -Path $Path -Description $Description
    try {
        $json = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    }
    catch {
        throw "$Description is not valid JSON: $resolved. $($_.Exception.Message)"
    }
    Assert-JsonNodePolicy -Node $json -Context $Description
    return $json
}

function Assert-GameManifestPolicy {
    param([string]$Path)

    $manifest = Read-StructuredPolicyJson `
        -Path $Path `
        -Description 'Game.manifest.json'
    $filesProperty = $manifest.PSObject.Properties['Files']
    if ($null -eq $filesProperty -or
        $null -eq $filesProperty.Value -or
        -not ($filesProperty.Value -is [Collections.IEnumerable])) {
        throw 'Game.manifest.json package policy requires a Files array.'
    }
}

function Assert-ZipPolicy {
    param([string]$Path, [string]$Description)

    $resolved = Assert-RegularPolicyFile -Path $Path -Description $Description
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($resolved)
    try {
        foreach ($entry in $archive.Entries) {
            Assert-PolicyPath `
                -Value $entry.FullName `
                -Context "$Description entry path"
            if ([string]::IsNullOrEmpty($entry.Name)) {
                continue
            }
            $entryStream = $entry.Open()
            try {
                Assert-PolicyStream `
                    -Stream $entryStream `
                    -Context "$Description entry '$($entry.FullName)'"
            }
            finally {
                $entryStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-DirectoryPolicy {
    param(
        [string]$Path,
        [string]$Description,
        [switch]$ScanContents
    )

    $root = Assert-RegularPolicyDirectory -Path $Path -Description $Description
    foreach ($entry in @(Get-ChildItem -LiteralPath $root -Force -Recurse)) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description contains a symbolic link or junction: $($entry.FullName)"
        }
        $relativePath = $entry.FullName.Substring($root.Length).TrimStart('\', '/')
        Assert-PolicyPath -Value $relativePath -Context "$Description path"
        if ($ScanContents -and -not $entry.PSIsContainer) {
            Assert-PolicyFileContent `
                -Path $entry.FullName `
                -Description "$Description file '$relativePath'" `
                -AllowEmpty
        }
    }
}

function Invoke-PackagePolicyVerification {
    $checkedSurfaceCount = 0
    $hasTargetGraph = -not [string]::IsNullOrWhiteSpace($TargetGraphPath)
    $hasLinkMap = -not [string]::IsNullOrWhiteSpace($LinkMapPath)
    if ($RequireTargetGraphAndLinkMap -and
        (-not $hasTargetGraph -or -not $hasLinkMap)) {
        throw 'Package policy requires both the CMake target graph and real shipping link map.'
    }
    if ($RequireBuildGraphInput -and -not ($hasTargetGraph -or $hasLinkMap)) {
        throw 'Package policy requires a target graph or link map for this invocation.'
    }

    if ($hasTargetGraph) {
        Assert-TargetGraphPolicy -Path $TargetGraphPath
        ++$checkedSurfaceCount
    }
    if ($hasLinkMap) {
        Assert-LinkMapPolicy -Path $LinkMapPath
        ++$checkedSurfaceCount
    }
    foreach ($path in @($MapPaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-PolicyFileContent -Path $path -Description 'shipping map file'
        ++$checkedSurfaceCount
    }
    foreach ($path in @($ExecutablePaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-PolicyFileContent -Path $path -Description 'shipping executable'
        ++$checkedSurfaceCount
    }
    foreach ($path in @($PdbPaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-PolicyFileContent -Path $path -Description 'shipping PDB'
        ++$checkedSurfaceCount
    }
    foreach ($path in @($ShaderManifestPaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        [void](Read-StructuredPolicyJson `
            -Path $path `
            -Description 'shipping shader manifest')
        ++$checkedSurfaceCount
    }
    foreach ($path in @($ShaderCacheDirectories)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-DirectoryPolicy `
            -Path $path `
            -Description 'shipping shader cache' `
            -ScanContents
        ++$checkedSurfaceCount
    }
    foreach ($path in @($GameManifestPaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-GameManifestPolicy -Path $path
        ++$checkedSurfaceCount
    }
    foreach ($path in @($GamePakPaths)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-ZipPolicy -Path $path -Description 'Game.pak'
        ++$checkedSurfaceCount
    }
    foreach ($path in @($PackageDirectories)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        $packageRoot = Assert-RegularPolicyDirectory `
            -Path $path `
            -Description 'final package directory'
        Assert-DirectoryPolicy `
            -Path $packageRoot `
            -Description 'final package directory'
        foreach ($binaryArtifact in @(Get-ChildItem `
                -LiteralPath $packageRoot `
                -Force `
                -Recurse `
                -File | Where-Object {
                    $_.Extension -ieq '.exe' -or
                    $_.Extension -ieq '.pdb' -or
                    $_.Extension -ieq '.map'
                })) {
            Assert-PolicyFileContent `
                -Path $binaryArtifact.FullName `
                -Description "final package artifact '$($binaryArtifact.Name)'"
        }
        $manifestPath = Join-Path $packageRoot 'Game.manifest.json'
        $pakPath = Join-Path $packageRoot 'Game.pak'
        Assert-GameManifestPolicy -Path $manifestPath
        Assert-ZipPolicy -Path $pakPath -Description 'Game.pak'
        ++$checkedSurfaceCount
    }
    foreach ($path in @($ExtractedRoots)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        Assert-DirectoryPolicy `
            -Path $path `
            -Description 'extracted package content' `
            -ScanContents
        ++$checkedSurfaceCount
    }

    if ($checkedSurfaceCount -eq 0) {
        throw 'Package policy verifier received no verification surface.'
    }
    if (-not $script:SuppressSuccess) {
        Write-Host "Verified package policy across $checkedSurfaceCount surface(s)."
    }
}

function Write-ContractText {
    param([string]$Path, [string]$Text)

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [IO.File]::WriteAllText(
        $Path,
        $Text,
        (New-Object Text.UTF8Encoding($false)))
}

function New-ContractZip {
    param([string]$Path, [string]$EntryPath, [string]$Payload)

    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Create,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
    $archive = New-Object IO.Compression.ZipArchive(
        $stream,
        [IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        $entry = $archive.CreateEntry($EntryPath)
        $entryStream = $entry.Open()
        $writer = New-Object IO.StreamWriter(
            $entryStream,
            (New-Object Text.UTF8Encoding($false)))
        try {
            $writer.Write($Payload)
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $archive.Dispose()
        $stream.Dispose()
    }
}

function Assert-ContractRejects {
    param([scriptblock]$Action, [string]$Description)

    $bRejected = $false
    try {
        & $Action
    }
    catch {
        $bRejected = $true
    }
    if (-not $bRejected) {
        throw "Package policy contract was not rejected: $Description"
    }
}

function Invoke-CMakeManifestContract {
    param([string]$ContractRoot)

    $repositoryRoot = [IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot '..\..'))
    $verifyScript = Join-Path $repositoryRoot 'CMake\VerifyGamePackage.cmake'
    $packageRoot = Join-Path $ContractRoot 'CMakePackage'
    $licenseRoot = Join-Path $packageRoot 'Licenses'
    $agilityRoot = Join-Path $packageRoot 'D3D12'
    [IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
    [IO.Directory]::CreateDirectory($agilityRoot) | Out-Null
    $requiredFiles = @(
        'Game.pak',
        'LICENSE.txt',
        'python311.dll',
        'README.txt',
        'FixtureGame.exe',
        'THIRD_PARTY_NOTICES.txt',
        'vcruntime140.dll'
    )
    Write-ContractText `
        -Path (Join-Path $packageRoot '.he-package-root') `
        -Text "HEngine fixture Package v1`n"
    foreach ($name in $requiredFiles) {
        Write-ContractText -Path (Join-Path $packageRoot $name) -Text 'fixture'
    }
    Write-ContractText `
        -Path (Join-Path $agilityRoot 'D3D12Core.dll') `
        -Text 'fixture'
    Write-ContractText `
        -Path (Join-Path $licenseRoot 'FixtureLicense.txt') `
        -Text 'fixture license'
    $licenseList = Join-Path $ContractRoot 'ExpectedPackageLicenses.txt'
    Write-ContractText -Path $licenseList -Text "FixtureLicense.txt`n"

    $requiredSettingsFiles = @(
        @{
            Path = 'Config/DefaultEngine.json'
            Type = 'Json'
            Dependencies = @()
            Tags = @()
        },
        @{
            Path = 'Config/BaseEngine.json'
            Type = 'Json'
            Dependencies = @()
            Tags = @()
        },
        @{
            Path = 'Shaders/CACHES/DXIL/Fixture.vs'
            Type = 'Shader'
            Dependencies = @()
            Tags = @()
        },
        @{
            Path = 'Shaders/CACHES/SPIRV/Fixture.vs'
            Type = 'Shader'
            Dependencies = @()
            Tags = @()
        },
        @{
            Path = 'Shaders/CACHES/DXIL/Materials/fixture/Fixture.ps'
            Type = 'Shader'
            Dependencies = @()
            Tags = @()
        },
        @{
            Path = 'Shaders/CACHES/SPIRV/Materials/fixture/Fixture.ps'
            Type = 'Shader'
            Dependencies = @()
            Tags = @()
        }
    )
    $cleanManifest = @{
        ManifestVersion = 1
        GameName = 'Fixture'
        EntryScene = 'Assets/Main.scene'
        PakFile = 'Game.pak'
        Files = $requiredSettingsFiles
    } | ConvertTo-Json -Depth 20
    $manifestPath = Join-Path $packageRoot 'Game.manifest.json'
    Write-ContractText -Path $manifestPath -Text $cleanManifest
    $cmake = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
    $arguments = @(
        "-DPACKAGE_DIR=$packageRoot",
        '-DPACKAGE_EXECUTABLE_NAME=FixtureGame',
        '-DPACKAGE_PROJECT_ID=fixture',
        "-DPACKAGE_LICENSE_LIST_FILE=$licenseList",
        '-P',
        $verifyScript
    )
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $cleanOutput = & $cmake @arguments 2>&1
        $cleanExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($cleanExitCode -ne 0) {
        throw "Clean VerifyGamePackage fixture failed: $($cleanOutput | Out-String)"
    }

    $forbiddenManifest = @{
        ManifestVersion = 1
        GameName = 'Fixture'
        EntryScene = 'Assets/Main.scene'
        PakFile = 'Game.pak'
        Files = @($requiredSettingsFiles) + @(
            @{
                Path = 'Config/DevOnly/Fixture.json'
                Type = 'Json'
                Dependencies = @()
                Tags = @()
            }
        )
    } | ConvertTo-Json -Depth 20
    Write-ContractText -Path $manifestPath -Text $forbiddenManifest
    try {
        $ErrorActionPreference = 'Continue'
        $forbiddenOutput = & $cmake @arguments 2>&1
        $forbiddenExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($forbiddenExitCode -eq 0) {
        throw "VerifyGamePackage accepted a forbidden structured manifest: $($forbiddenOutput | Out-String)"
    }

    $userSettingsManifest = @{
        ManifestVersion = 1
        GameName = 'Fixture'
        EntryScene = 'Assets/Main.scene'
        PakFile = 'Game.pak'
        Files = @($requiredSettingsFiles) + @(
            @{
                Path = 'Config/UserEngine.json'
                Type = 'Json'
                Dependencies = @()
                Tags = @()
            }
        )
    } | ConvertTo-Json -Depth 20
    Write-ContractText -Path $manifestPath -Text $userSettingsManifest
    try {
        $ErrorActionPreference = 'Continue'
        $userSettingsOutput = & $cmake @arguments 2>&1
        $userSettingsExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($userSettingsExitCode -eq 0) {
        throw "VerifyGamePackage accepted Config/UserEngine.json: $($userSettingsOutput | Out-String)"
    }
    Write-ContractText -Path $manifestPath -Text $cleanManifest
}

function Invoke-PackagePolicyContract {
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $contractId = 'HEngine-PackagePolicyContract-' + [Guid]::NewGuid().ToString('N')
    $contractRoot = Join-Path $temporaryRoot $contractId
    [IO.Directory]::CreateDirectory($contractRoot) | Out-Null
    $script:SuppressSuccess = $true
    try {
        $graphPath = Join-Path $contractRoot 'PackageTargetGraph.dot'
        $linkMapPath = Join-Path $contractRoot 'HGame.map'
        $exePath = Join-Path $contractRoot 'FixtureGame.exe'
        $pdbPath = Join-Path $contractRoot 'FixtureGame.pdb'
        $shaderManifestPath = Join-Path $contractRoot 'ShippingShaders.json'
        $shaderCache = Join-Path $contractRoot 'ShaderCache'
        $gameManifestPath = Join-Path $contractRoot 'Game.manifest.json'
        $gamePakPath = Join-Path $contractRoot 'Game.pak'
        $packageRoot = Join-Path $contractRoot 'FinalPackage'
        $extractedRoot = Join-Path $contractRoot 'Extracted'
        [IO.Directory]::CreateDirectory($shaderCache) | Out-Null
        [IO.Directory]::CreateDirectory($packageRoot) | Out-Null
        [IO.Directory]::CreateDirectory($extractedRoot) | Out-Null

        Write-ContractText -Path $graphPath -Text 'digraph { HGame -> HEngineRuntime }'
        Write-ContractText `
            -Path $linkMapPath `
            -Text 'HEngineRuntime.lib(SceneRenderer.cpp.obj) shipping link map'
        Write-ContractText -Path $exePath -Text 'shipping executable'
        Write-ContractText -Path $pdbPath -Text 'shipping symbols'
        Write-ContractText `
            -Path $shaderManifestPath `
            -Text (@{ Artifacts = @(@{ Path = 'DXIL/SceneOpaque.cs' }) } |
                ConvertTo-Json -Depth 10)
        Write-ContractText `
            -Path (Join-Path $shaderCache 'SceneOpaque.dxil') `
            -Text 'shader artifact'
        $cleanGameManifest = @{
            ManifestVersion = 1
            GameName = 'Fixture'
            EntryScene = 'Assets/Main.scene'
            PakFile = 'Game.pak'
            Files = @(@{
                Path = 'Assets/Main.scene'
                Type = 'Scene'
                Dependencies = @()
                Tags = @()
            })
        } | ConvertTo-Json -Depth 20
        Write-ContractText -Path $gameManifestPath -Text $cleanGameManifest
        New-ContractZip `
            -Path $gamePakPath `
            -EntryPath 'Assets/Main.scene' `
            -Payload 'scene payload'
        Write-ContractText `
            -Path (Join-Path $packageRoot 'FixtureGame.exe') `
            -Text 'shipping executable'
        Write-ContractText `
            -Path (Join-Path $packageRoot 'Game.manifest.json') `
            -Text $cleanGameManifest
        New-ContractZip `
            -Path (Join-Path $packageRoot 'Game.pak') `
            -EntryPath 'Assets/Main.scene' `
            -Payload 'scene payload'
        Write-ContractText `
            -Path (Join-Path $extractedRoot 'Assets\Main.scene') `
            -Text 'scene payload'

        $savedTargetGraphPath = $TargetGraphPath
        $savedLinkMapPath = $LinkMapPath
        $savedExecutablePaths = $ExecutablePaths
        $savedPdbPaths = $PdbPaths
        $savedShaderManifestPaths = $ShaderManifestPaths
        $savedShaderCacheDirectories = $ShaderCacheDirectories
        $savedGameManifestPaths = $GameManifestPaths
        $savedGamePakPaths = $GamePakPaths
        $savedPackageDirectories = $PackageDirectories
        $savedExtractedRoots = $ExtractedRoots
        try {
            $script:TargetGraphPath = $graphPath
            $script:LinkMapPath = $linkMapPath
            $script:ExecutablePaths = @($exePath)
            $script:PdbPaths = @($pdbPath)
            $script:ShaderManifestPaths = @($shaderManifestPath)
            $script:ShaderCacheDirectories = @($shaderCache)
            $script:GameManifestPaths = @($gameManifestPath)
            $script:GamePakPaths = @($gamePakPath)
            $script:PackageDirectories = @($packageRoot)
            $script:ExtractedRoots = @($extractedRoot)
            Invoke-PackagePolicyVerification
        }
        finally {
            $script:TargetGraphPath = $savedTargetGraphPath
            $script:LinkMapPath = $savedLinkMapPath
            $script:ExecutablePaths = $savedExecutablePaths
            $script:PdbPaths = $savedPdbPaths
            $script:ShaderManifestPaths = $savedShaderManifestPaths
            $script:ShaderCacheDirectories = $savedShaderCacheDirectories
            $script:GameManifestPaths = $savedGameManifestPaths
            $script:GamePakPaths = $savedGamePakPaths
            $script:PackageDirectories = $savedPackageDirectories
            $script:ExtractedRoots = $savedExtractedRoots
        }

        Assert-ContractRejects {
            $TargetGraphPath = ''
            $LinkMapPath = ''
            $RequireBuildGraphInput = $true
            Invoke-PackagePolicyVerification
        } 'required build graph input'
        Assert-ContractRejects {
            $TargetGraphPath = $graphPath
            $LinkMapPath = ''
            $RequireTargetGraphAndLinkMap = $true
            Invoke-PackagePolicyVerification
        } 'required shipping link map'
        Assert-ContractRejects {
            $TargetGraphPath = ''
            $LinkMapPath = Join-Path $contractRoot 'HGame.map'
            $RequireTargetGraphAndLinkMap = $true
            Invoke-PackagePolicyVerification
        } 'required CMake target graph'

        Write-ContractText `
            -Path $graphPath `
            -Text 'digraph { HGame -> HEngineRuntime; HGame -> OfflineReferenceRenderer }'
        Assert-ContractRejects {
            Assert-PolicyFileContent $graphPath 'target graph fixture'
        } 'target graph token'
        Write-ContractText -Path $graphPath -Text 'digraph { HGame -> HEngineRuntime }'

        Write-ContractText `
            -Path $linkMapPath `
            -Text 'HEngineRuntime.lib(ReferencePathTracer.obj)'
        Assert-ContractRejects {
            Assert-PolicyFileContent $linkMapPath 'link map fixture'
        } 'link map token'
        Write-ContractText `
            -Path $linkMapPath `
            -Text 'HEngineRuntime.lib(SceneRenderer.cpp.obj) shipping link map'

        Write-ContractText -Path $exePath -Text "binary`0HardwareRayTracingReference`0"
        Assert-ContractRejects {
            Assert-PolicyFileContent $exePath 'executable fixture'
        } 'EXE token'
        [IO.File]::WriteAllBytes(
            $exePath,
            [Text.Encoding]::Unicode.GetBytes('OfflineReferenceRenderer'))
        Assert-ContractRejects {
            Assert-PolicyFileContent $exePath 'UTF-16 executable fixture'
        } 'UTF-16 EXE token'
        Write-ContractText -Path $exePath -Text 'shipping executable'

        Write-ContractText -Path $pdbPath -Text 'PackageNever symbol'
        Assert-ContractRejects {
            Assert-PolicyFileContent $pdbPath 'PDB fixture'
        } 'PDB token'
        Write-ContractText -Path $pdbPath -Text 'shipping symbols'

        $forbiddenPackageMap = Join-Path $packageRoot 'PackageNever.map'
        Write-ContractText -Path $forbiddenPackageMap -Text 'shipping map'
        Assert-ContractRejects {
            Assert-DirectoryPolicy $packageRoot 'final package fixture'
        } 'final package path token'
        Remove-Item -LiteralPath $forbiddenPackageMap -Force

        Write-ContractText `
            -Path $shaderManifestPath `
            -Text (@{ Artifacts = @(@{ Source = 'HardwareRayTracingReference.hlsl' }) } |
                ConvertTo-Json -Depth 10)
        Assert-ContractRejects {
            [void](Read-StructuredPolicyJson $shaderManifestPath 'shader manifest fixture')
        } 'structured shader manifest token'

        $forbiddenShader = Join-Path $shaderCache 'OfflineReferenceRenderer.dxil'
        Write-ContractText -Path $forbiddenShader -Text 'shader artifact'
        Assert-ContractRejects {
            Assert-DirectoryPolicy $shaderCache 'shader cache fixture' -ScanContents
        } 'shader cache token'
        Remove-Item -LiteralPath $forbiddenShader -Force

        $forbiddenGameManifest = @{
            ManifestVersion = 1
            Files = @(@{
                Path = 'Shaders/DevOnly/Fixture.dxil'
                Dependencies = @(@{ Path = 'Config/DevOnly/Fixture.json' })
                Tags = @()
            })
        } | ConvertTo-Json -Depth 20
        Write-ContractText -Path $gameManifestPath -Text $forbiddenGameManifest
        Assert-ContractRejects {
            Assert-GameManifestPolicy $gameManifestPath
        } 'structured Game.manifest.json prefix'
        Write-ContractText `
            -Path $gameManifestPath `
            -Text (@{
                ManifestVersion = 1
                Files = @(@{ Path = 'Config/Plugins.json'; Tags = @() })
            } | ConvertTo-Json -Depth 20)
        Assert-ContractRejects {
            Assert-GameManifestPolicy $gameManifestPath
        } 'project plugin host config in Game.manifest.json'
        Write-ContractText -Path $gameManifestPath -Text $cleanGameManifest

        Remove-Item -LiteralPath $gamePakPath -Force
        New-ContractZip `
            -Path $gamePakPath `
            -EntryPath 'Assets/Main.scene' `
            -Payload 'ReferencePathTracer payload'
        Assert-ContractRejects {
            Assert-ZipPolicy $gamePakPath 'Game.pak payload fixture'
        } 'Game.pak payload token'
        Remove-Item -LiteralPath $gamePakPath -Force
        New-ContractZip `
            -Path $gamePakPath `
            -EntryPath 'Shaders/DevOnly/Fixture.dxil' `
            -Payload 'shader payload'
        Assert-ContractRejects {
            Assert-ZipPolicy $gamePakPath 'Game.pak fixture'
        } 'Game.pak entry prefix'

        $forbiddenExtracted = Join-Path $extractedRoot 'Config\DevOnly\Fixture.json'
        Write-ContractText -Path $forbiddenExtracted -Text '{}'
        Assert-ContractRejects {
            Assert-DirectoryPolicy $extractedRoot 'extracted fixture' -ScanContents
        } 'extracted Config/DevOnly prefix'

        Invoke-CMakeManifestContract -ContractRoot $contractRoot
        Write-Host 'Package policy fixture contract PASS'
    }
    finally {
        if (Test-Path -LiteralPath $contractRoot) {
            $resolvedRoot = [IO.Path]::GetFullPath($contractRoot).TrimEnd('\', '/')
            $resolvedParent = [IO.Path]::GetFullPath(
                (Split-Path -Parent $resolvedRoot)).TrimEnd('\', '/')
            $resolvedLeaf = Split-Path -Leaf $resolvedRoot
            if ($resolvedParent -cne $temporaryRoot -or
                $resolvedLeaf -cne $contractId) {
                throw "Refusing to remove unexpected package policy fixture root: $resolvedRoot"
            }
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

if ($Contract) {
    Invoke-PackagePolicyContract
}
else {
    Invoke-PackagePolicyVerification
}
