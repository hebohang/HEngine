param(
    [string]$Project = "",
    [ValidatePattern('^$|^[A-Za-z0-9_./-]+[.]json$')][string]$GameConfig = "",
    [string]$PluginConfig = "",
    [string]$OutputRoot = "",
    [ValidateSet('Desktop','Portable')][string]$ScriptQuality = 'Desktop',
    [switch]$PortablePlayer,
    [ValidateSet('', 'x64', 'arm64')][string]$Architecture = ''
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Toolchain.ps1")
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$generatorPlatform = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x64' }
$toolchain = Get-HeVisualStudio -Architecture $Architecture
Import-HeToolchain -Toolchain $toolchain

function Get-PathHash {
    param([string]$Value)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
        return ([System.BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

function Invoke-Native {
    param(
        [string]$Description,
        [scriptblock]$Command
    )

    $nativeExit = 1
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        if ($packageEvidenceDirectory) {
            & $Command *>&1 | Tee-Object -FilePath (Join-Path $packageEvidenceDirectory (($Description -replace '[^A-Za-z0-9-]', '-') + '.log'))
        }
        else { & $Command }
        $nativeExit = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) {
        throw "$Description failed with exit code $nativeExit."
    }
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($Project)) {
    . (Join-Path $PSScriptRoot 'Project.ps1')
    $Project = (Resolve-HEProject -RepositoryRoot $repositoryRoot).Root
}
elseif (-not [System.IO.Path]::IsPathRooted($Project) -and
    -not (Test-Path -LiteralPath $Project)) {
    $repositoryRelativeProject = Join-Path $repositoryRoot $Project
    if (Test-Path -LiteralPath $repositoryRelativeProject) {
        $Project = $repositoryRelativeProject
    }
}

$projectItem = Get-Item -LiteralPath (Resolve-Path -LiteralPath $Project).Path -Force
if ($projectItem.PSIsContainer) {
    $projectRoot = $projectItem.FullName
    $descriptorPath = Join-Path $projectRoot "Project.heproject"
}
else {
    if ($projectItem.Name -ine "Project.heproject") {
        throw "-Project must name a project directory or Project.heproject: $($projectItem.FullName)"
    }
    $descriptorPath = $projectItem.FullName
    $projectRoot = $projectItem.Directory.FullName
}
if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
    throw "Project descriptor was not found: $descriptorPath"
}

try {
    $descriptor = Get-Content -Raw -LiteralPath $descriptorPath -Encoding UTF8 | ConvertFrom-Json
}
catch {
    throw "Project descriptor is not valid JSON: $descriptorPath. $($_.Exception.Message)"
}
$projectId = [string]$descriptor.ProjectId
$executableName = [string]$descriptor.ExecutableName
if ($projectId.Length -gt 128 -or
    $projectId -notmatch '^[a-z0-9][a-z0-9._-]*$' -or
    $projectId.EndsWith('.') -or
    $projectId -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
    throw "Invalid ProjectId in Project.heproject: $descriptorPath"
}
if ($executableName.Length -gt 64 -or
    $executableName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
    $executableName.EndsWith('.') -or
    $executableName -match '(?i)\.exe$' -or
    $executableName -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
    throw "Invalid ExecutableName in Project.heproject: $descriptorPath"
}

$projectRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/')
$defaultProjectRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "Project")).TrimEnd('\', '/')
$packageCacheHash = (Get-PathHash -Value "${projectId}`n${projectRoot}").Substring(0, 12)
if ($PortablePlayer) { $packageCacheHash = (Get-PathHash -Value "${projectId}`n${projectRoot}`nportable-player").Substring(0, 12) }
if ($Architecture -ne 'x64') { $packageCacheHash = (Get-PathHash -Value "${packageCacheHash}`n$Architecture").Substring(0, 12) }
$bDefaultProject = [string]::Equals(
    $projectRoot,
    $defaultProjectRoot,
    [System.StringComparison]::OrdinalIgnoreCase)
$packageOutputIdentity = "${projectId}`n${projectRoot}"
if ($PortablePlayer) { $packageOutputIdentity += "`nportable-player" }
if ($Architecture -ne 'x64') { $packageOutputIdentity += "`n$Architecture" }
if ($GameConfig) { $packageOutputIdentity += "`n${GameConfig}" }
if ($PluginConfig) {
    $PluginConfig = (Resolve-Path -LiteralPath $PluginConfig -ErrorAction Stop).Path
    $pluginBytes = [IO.File]::ReadAllBytes($PluginConfig)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $pluginIdentity = [BitConverter]::ToString($hasher.ComputeHash($pluginBytes)).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
    $packageOutputIdentity += "`nplugins:${pluginIdentity}"
}
$packageOutputKey = if ($bDefaultProject -and !$GameConfig -and !$PluginConfig -and !$PortablePlayer) {
    ""
}
else {
    (Get-PathHash -Value $packageOutputIdentity).Substring(0, 16)
}
$distributionRoot = if ($OutputRoot) { [IO.Path]::GetFullPath($OutputRoot) } else { Join-Path $repositoryRoot 'dist' }
$packageOutputRoot = if ($bDefaultProject -and !$GameConfig -and !$PluginConfig -and !$PortablePlayer) {
    $distributionRoot
}
else {
    Join-Path $distributionRoot "projects\$packageOutputKey"
}
$buildDir = if ($bDefaultProject -and !$PortablePlayer) {
    Join-Path $repositoryRoot "out\package\build"
}
else {
    # Keep the physical Release build root short enough for MSBuild's legacy
    # MAX_PATH-sensitive .tlog/.lastbuildstate files. The hash still includes
    # both the stable project identity and canonical project path.
    Join-Path $repositoryRoot "out\pkg\$packageCacheHash"
}
$generatedSourceDir = Join-Path $buildDir "GeneratedSource"
$packageDir = Join-Path $packageOutputRoot $executableName

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$legacyLockDirectory = Join-Path $buildDir ".package-build.lock"
if (Test-Path -LiteralPath $legacyLockDirectory -PathType Container) {
    throw "A legacy package lock directory remains at $legacyLockDirectory. Confirm that no package build is running, then remove this empty directory once."
}
$lockPath = Join-Path $buildDir ".package-build.lock"
. (Join-Path $PSScriptRoot 'Storage.ps1')
$packageStorageRuns = @()
$packageEvidenceRun = $null
$packageEvidenceDirectory = $null
$packageStorageState = 'failed'
$cacheLease = $null
$packageLock = $null
$previousNodeReuse = $env:MSBUILDDISABLENODEREUSE
try {
    $cacheLease = Open-HECacheLease -CachePath $buildDir
    try {
        $packageLock = [System.IO.File]::Open(
            $lockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another package build is using $buildDir. Wait for it to finish."
    }

    $packageEvidenceDirectory = Join-Path $repositoryRoot ('out/agent/logs/package-run-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $packageEvidenceDirectory -Force | Out-Null
    $packageEvidenceRun = Start-HEStorageRun -Project $projectRoot -Workflow 'package' -Paths @($packageEvidenceDirectory) -Locks @($lockPath) -Outer
    if ($PluginConfig) {
        # Compile exactly the bytes used to choose this package's output identity.
        $PluginConfig = Join-Path $packageEvidenceDirectory 'Plugins.json'
        [IO.File]::WriteAllBytes($PluginConfig, $pluginBytes)
    }
    $env:MSBUILDDISABLENODEREUSE = "1"
    $cmakeCommand = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
    $freshConfigure = @(Get-HeFreshConfigureArguments -BuildRoot $buildDir -Compiler $toolchain.Compiler -Generator $toolchain.Generator)
    Write-Host "toolchain: $($toolchain.Generator), $($toolchain.Toolset), MSVC $($toolchain.ToolsVersion)"
    Invoke-Native -Description "Package configure" -Command {
        & $cmakeCommand @freshConfigure -S $repositoryRoot -B $buildDir `
            "-DHE_PROJECT_DIR=$projectRoot" `
            "-DHE_GENERATED_SOURCE_DIR=$generatedSourceDir" `
            "-DHE_PACKAGE_OUTPUT_KEY=$packageOutputKey" `
            "-DHE_PACKAGE_DISTRIBUTION_ROOT=$distributionRoot" `
            "-DHE_PACKAGE_GAME_CONFIG=$GameConfig" `
            "-DHE_PLUGIN_CONFIG=$PluginConfig" `
            "-DHE_CODEGEN_LIBCLANG_ROOT=$env:HE_CODEGEN_LIBCLANG_ROOT" `
            "-DHE_CODEGEN_PARSER_EXECUTABLE=$env:HE_CODEGEN_PARSER_EXECUTABLE" `
            -DPACK_GAME=ON `
            "-DHE_PORTABLE_PLAYER=$(if ($PortablePlayer) { 'ON' } else { 'OFF' })" `
            "-DHE_SCRIPT_QUALITY=$ScriptQuality" `
            -DHE_BUILD_DEV_GAME=OFF `
            -DHE_ENABLE_GM=OFF `
            -DHE_BUILD_TESTS=OFF `
            -DHE_CODEGEN_HOST_OVERRIDE=Package `
            -DCPP_CODE_GEN=OFF `
            -DHE_ENABLE_TRACY=OFF `
            -DBUILD_SHARED_LIBS=OFF `
            -DGLFW_LIBRARY_TYPE=STATIC `
            "-DCMAKE_GENERATOR_INSTANCE=$($toolchain.Path)" `
            -G $toolchain.Generator -A $generatorPlatform -T $toolchain.Toolset
    }
    $packageTargetGraph = Join-Path $buildDir "PackageTargetGraph.dot"
    $expectedPackageLinkMap = Join-Path $buildDir `
        "PackagePolicy\HGame-Release.map"
    $packageLinkMapDeclaration = Join-Path $buildDir "PackageLinkMap.path"
    if (-not (Test-Path -LiteralPath $packageLinkMapDeclaration -PathType Leaf)) {
        throw "Package configure did not declare the shipping link map path: $packageLinkMapDeclaration"
    }
    $packageLinkMap = (Get-Content -LiteralPath $packageLinkMapDeclaration -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($packageLinkMap) -or
        -not [IO.Path]::IsPathRooted($packageLinkMap) -or
        -not [string]::Equals(
            [IO.Path]::GetFullPath($packageLinkMap),
            [IO.Path]::GetFullPath($expectedPackageLinkMap),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package configure declared an unexpected shipping link map path: '$packageLinkMap'"
    }
    Invoke-Native -Description "Package target graph generation" -Command {
        & $cmakeCommand "--graphviz=$packageTargetGraph" $buildDir
    }
    $packagePolicyVerifier = Join-Path $repositoryRoot `
        "Tools\Packaging\Verify-PackagePolicy.ps1"
    if (-not (Test-Path -LiteralPath $packagePolicyVerifier -PathType Leaf)) {
        throw "Package policy verifier is missing: $packagePolicyVerifier"
    }
    & $packagePolicyVerifier `
        -TargetGraphPath $packageTargetGraph `
        -RequireBuildGraphInput
    $storagePaths = @(Get-Content -LiteralPath (Join-Path $buildDir 'PackageStoragePaths.txt'))
    if ($storagePaths.Count -ne 2) { throw 'Invalid package storage path declaration.' }
    $packageStorageRuns += Start-HEStorageRun -Project $projectRoot -ProjectId $projectId -Workflow 'package-cook' -Kind 'package-intermediate' -Paths @($storagePaths[0]) -Locks @($lockPath)
    $packageStorageRuns += Start-HEStorageRun -Project $projectRoot -ProjectId $projectId -Workflow 'package-staging' -Kind 'package-staging' -Paths @($storagePaths[1]) -Locks @($lockPath)
    Invoke-Native -Description "Package build" -Command {
        & $cmakeCommand --build $buildDir --config Release --target PackageGame --parallel 2
    }

    # PackageGame verifies the target graph, shipping link map, staged package,
    # and full WARP start before its final publish command. Keep every fallible
    # package gate inside that pre-publish boundary.

    $packageStorageState = 'success'
    Write-Host "Packaged project: $projectId"
    Write-Host "Packaged game: $(Join-Path $packageDir ($executableName + '.exe'))"
}
finally {
    if ($packageEvidenceRun) {
        try {
            foreach ($packageEvidence in @('PackageStoragePaths.txt', 'Engine/Source/Game/CookReport.json')) {
                $sourceEvidence = Join-Path $buildDir $packageEvidence
                if (Test-Path -LiteralPath $sourceEvidence) { Copy-Item -LiteralPath $sourceEvidence -Destination $packageEvidenceDirectory }
            }
            [ordered]@{project=$projectRoot;projectId=$projectId;buildRoot=$buildDir;package=$packageDir;state=$packageStorageState} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $packageEvidenceDirectory 'result.json') -Encoding UTF8
        }
        # A diagnostic copy can fail when the disk is full; it does not make the
        # already observed package process outcome unknown.
        catch { Write-Warning "storage package evidence snapshot failed: $($_.Exception.Message)" }
    }
    if ($null -ne $packageLock) {
        $packageLock.Dispose()
    }
    if ($cacheLease) { $cacheLease.Dispose() }
    foreach ($storageRun in $packageStorageRuns) { Complete-HEStorageRun -Run $storageRun -State $packageStorageState -NoAuto }
    Complete-HEStorageRun -Run $packageEvidenceRun -State $packageStorageState -NoAuto
    if ($packageStorageState -ne 'success') { Invoke-HEStorageAuto }
    if ($null -eq $previousNodeReuse) {
        Remove-Item Env:\MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue
    }
    else {
        $env:MSBUILDDISABLENODEREUSE = $previousNodeReuse
    }
}

# Successful CookInput copies are disposable immediately, even inside the normal throttle interval.
try { $null = Invoke-HEStorage @('stale','--kind','package-intermediate','--project',$projectRoot) }
catch { Write-Warning "storage package cleanup deferred: $($_.Exception.Message)" }
Invoke-HEStorageAuto
