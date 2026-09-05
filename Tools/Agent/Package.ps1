param(
    [string]$Project = "",

    [string]$ResultFile = ""
)

$ErrorActionPreference = "Stop"

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

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($Project)) {
    $Project = Join-Path $repositoryRoot "Project"
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
    $descriptor = Get-Content -Raw -LiteralPath $descriptorPath | ConvertFrom-Json
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
$bDefaultProject = [string]::Equals(
    $projectRoot,
    $defaultProjectRoot,
    [System.StringComparison]::OrdinalIgnoreCase)
$packageOutputKey = if ($bDefaultProject) {
    ""
}
else {
    (Get-PathHash -Value "${projectId}`n${projectRoot}").Substring(0, 16)
}
$packageOutputRoot = if ($bDefaultProject) {
    Join-Path $repositoryRoot "dist"
}
else {
    Join-Path $repositoryRoot "dist\projects\$packageOutputKey"
}
$buildDir = if ($bDefaultProject) {
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
$packageLock = $null
$previousNodeReuse = $env:MSBUILDDISABLENODEREUSE
try {
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

    $env:MSBUILDDISABLENODEREUSE = "1"
    $cmakeCommand = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
    Invoke-Native -Description "Package configure" -Command {
        & $cmakeCommand -S $repositoryRoot -B $buildDir `
            "-DHE_PROJECT_DIR=$projectRoot" `
            "-DHE_GENERATED_SOURCE_DIR=$generatedSourceDir" `
            "-DHE_PACKAGE_OUTPUT_KEY=$packageOutputKey" `
            -DPACK_GAME=ON `
            -DHE_BUILD_DEV_GAME=OFF `
            -DHE_ENABLE_GM=OFF `
            -DHE_BUILD_TESTS=OFF `
            -DHE_CODEGEN_HOST_OVERRIDE=Package `
            -DCPP_CODE_GEN=OFF `
            -DHE_ENABLE_TRACY=OFF `
            -DBUILD_SHARED_LIBS=OFF `
            -DGLFW_LIBRARY_TYPE=STATIC `
            -G "Visual Studio 17 2022" -A x64 -T v143
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
    Invoke-Native -Description "Package build" -Command {
        & $cmakeCommand --build $buildDir --config Release --target PackageGame --parallel 2
    }

    # PackageGame verifies the target graph, shipping link map, staged package,
    # and full WARP start before its final publish command. Keep every fallible
    # package gate inside that pre-publish boundary.

    if (-not [string]::IsNullOrWhiteSpace($ResultFile)) {
        $resultPath = [System.IO.Path]::GetFullPath($ResultFile)
        $resultDirectory = Split-Path -Parent $resultPath
        New-Item -ItemType Directory -Force -Path $resultDirectory | Out-Null
        $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($resultPath, $packageDir, $utf8WithoutBom)
    }

    Write-Host "Packaged project: $projectId"
    Write-Host "Packaged game: $(Join-Path $packageDir ($executableName + '.exe'))"
}
finally {
    if ($null -ne $packageLock) {
        $packageLock.Dispose()
    }
    if ($null -eq $previousNodeReuse) {
        Remove-Item Env:\MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue
    }
    else {
        $env:MSBUILDDISABLENODEREUSE = $previousNodeReuse
    }
}
