param(
    [ValidateSet(
        "HGame",
        "HEngineEditor",
        "HEngineParser",
        "HAssetImporter",
        "HCooker",
        "HShaderCompiler",
        "HEngineTests",
        "HEngineDX12SceneSmoke",
        "HEngineRmlDX12Smoke")]
    [string]$Target = "HGame",

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [switch]$Run,

    [switch]$Wait,

    [ValidateRange(0, 3600)]
    [int]$TimeoutSeconds = 0,

    [ValidateRange(0, 10000000)]
    [long]$ExitAfterFrames = 0,

    [double]$FixedDeltaSeconds = 0.0,

    [string]$Resolution = "",

    [ValidateRange(0, 10000000)]
    [long]$CaptureFrame = 0,

    [string]$CapturePath = "",

    [string]$StartupScene = "",

    [switch]$NoCursorCapture,

    [switch]$NoVSync,

    [switch]$RenderValidation,

    [switch]$RenderStats,

    [ValidateSet("", "DX12", "Vulkan")]
    [string]$GraphicsApi = "",

    [switch]$StrictGraphicsApi,

    [switch]$UseWarp,

    [switch]$EnableTracy,

    [switch]$PortableEditor,

    [string]$Project = "",

    [switch]$ShowBuildOutput
)

$ErrorActionPreference = "Stop"
if ($UseWarp -and $GraphicsApi -eq "Vulkan") {
    throw "-UseWarp is valid only with -GraphicsApi DX12."
}
. (Join-Path $PSScriptRoot "Process.ps1")

function Get-RepositoryRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

function Resolve-HEProject {
    param(
        [string]$ProjectPath,
        [string]$RepositoryRoot
    )

    if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
        $ProjectPath = $env:HE_PROJECT_DIR
    }
    if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
        $ProjectPath = Join-Path $RepositoryRoot "Project"
    }
    elseif (-not [System.IO.Path]::IsPathRooted($ProjectPath) -and
        -not (Test-Path -LiteralPath $ProjectPath)) {
        $repositoryRelativePath = Join-Path $RepositoryRoot $ProjectPath
        if (Test-Path -LiteralPath $repositoryRelativePath) {
            $ProjectPath = $repositoryRelativePath
        }
    }

    $resolvedInput = Resolve-Path -LiteralPath $ProjectPath -ErrorAction Stop
    $inputItem = Get-Item -LiteralPath $resolvedInput.Path -Force
    if ($inputItem.PSIsContainer) {
        $projectRoot = $inputItem.FullName
        $descriptorPath = Join-Path $projectRoot "Project.heproject"
    }
    else {
        if ($inputItem.Name -ine "Project.heproject") {
            throw "-Project must name a project directory or Project.heproject: $($inputItem.FullName)"
        }
        $descriptorPath = $inputItem.FullName
        $projectRoot = $inputItem.Directory.FullName
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
        throw "Project.heproject ProjectId must use lowercase ASCII letters, digits, '.', '_' or '-': $descriptorPath"
    }
    if ($executableName.Length -gt 64 -or
        $executableName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
        $executableName.EndsWith('.') -or $executableName -match '(?i)\.exe$') {
        throw "Project.heproject ExecutableName must be a safe extension-free executable basename: $descriptorPath"
    }
    if ($executableName -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
        throw "Project.heproject ExecutableName uses a reserved Windows filename: $descriptorPath"
    }

    $normalizedRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/')
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $rootBytes = [System.Text.Encoding]::UTF8.GetBytes($normalizedRoot.ToLowerInvariant())
        $pathHash = ([System.BitConverter]::ToString($sha256.ComputeHash($rootBytes))).Replace('-', '').Substring(0, 8).ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }

    return [pscustomobject]@{
        Root = $normalizedRoot
        DescriptorPath = [System.IO.Path]::GetFullPath($descriptorPath)
        ProjectId = $projectId
        ExecutableName = $executableName
        CacheKey = "${projectId}-${pathHash}"
    }
}

function Test-DefaultProject {
    param(
        [string]$ProjectRoot,
        [string]$RepositoryRoot
    )

    $defaultRoot = [System.IO.Path]::GetFullPath((Join-Path $RepositoryRoot "Project")).TrimEnd('\', '/')
    return [string]::Equals(
        $ProjectRoot,
        $defaultRoot,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Import-Vs2022Environment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe was not found; install the Visual Studio 2022 C++ toolchain."
    }

    $installationPath = & $vswhere -latest -version "[17.0,18.0)" -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
        throw "A Visual Studio 2022 installation with the x64 C++ toolchain was not found."
    }

    # Ninja's MSVC dependency scanner matches the English /showIncludes prefix.
    # Keep compiler diagnostics deterministic so header dependencies are recorded
    # even when the Windows display language is not English.
    [Environment]::SetEnvironmentVariable(
        "VSLANG",
        "1033",
        [EnvironmentVariableTarget]::Process)

    $devCommand = Join-Path ($installationPath | Select-Object -First 1) "Common7\Tools\VsDevCmd.bat"
    $environmentLines = & $env:ComSpec /d /s /c `
        "`"$devCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed to initialize the x64 toolchain."
    }

    foreach ($line in $environmentLines) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable(
                $matches[1],
                $matches[2],
                [EnvironmentVariableTarget]::Process)
        }
    }

    # Some launch hosts expose both PATH and Path. VsDevCmd updates Path, and
    # PowerShell Start-Process rejects the resulting case-insensitive duplicate
    # environment block. Preserve the toolchain-updated value under one key.
    $processEnvironment =
        [Environment]::GetEnvironmentVariables([EnvironmentVariableTarget]::Process)
    $canonicalPath = $processEnvironment['Path']
    if ([string]::IsNullOrWhiteSpace($canonicalPath)) {
        $canonicalPath = $processEnvironment['PATH']
    }
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    Remove-Item Env:Path -ErrorAction SilentlyContinue
    $env:PATH = $canonicalPath
}

function Invoke-LoggedCommand {
    param(
        [string]$Label,
        [string]$LogPath,
        [scriptblock]$Command
    )

    $startedAt = Get-Date
    $exitCode = 1
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell converts native stderr into ErrorRecord objects. Keep
        # those diagnostics in the log without treating ordinary tool output as
        # a terminating PowerShell error.
        $ErrorActionPreference = "Continue"
        if ($ShowBuildOutput) {
            Write-Host ("{0}: START; live output is also logged to {1}" -f $Label, $LogPath)
            & $Command *>&1 | Tee-Object -FilePath $LogPath
        }
        else {
            & $Command *> $LogPath
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $elapsed = (Get-Date) - $startedAt
    if ($exitCode -ne 0) {
        Write-Host ("{0}: FAIL ({1:n1}s); tail of {2}" -f $Label, $elapsed.TotalSeconds, $LogPath)
        Get-Content -LiteralPath $LogPath -Tail 100
        exit $exitCode
    }
    Write-Host ("{0}: PASS ({1:n1}s); log: {2}" -f $Label, $elapsed.TotalSeconds, $LogPath)
}

function Test-CMakeCacheValue {
    param(
        [string]$Path,
        [string]$Name,
        [string]$ExpectedValue
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    $escapedName = [regex]::Escape($Name)
    $match = Select-String -LiteralPath $Path -Pattern "^${escapedName}:[^=]+=(.*)$" |
        Select-Object -First 1
    if ($null -eq $match) {
        return $false
    }
    return [string]::Equals(
        $match.Matches[0].Groups[1].Value,
        $ExpectedValue,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-StableArtifactNotRunning {
    param(
        [string]$ArtifactPath,
        [string]$ProcessName
    )

    if ([string]::IsNullOrWhiteSpace($ArtifactPath) -or
        -not (Test-Path -LiteralPath $ArtifactPath)) {
        return
    }
    $expectedPath = [System.IO.Path]::GetFullPath($ArtifactPath)
    foreach ($candidate in Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        try {
            $candidatePath = $candidate.Path
        }
        catch {
            continue
        }
        if ($candidatePath -and [string]::Equals(
            [System.IO.Path]::GetFullPath($candidatePath),
            $expectedPath,
            [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Close the running $ProcessName process before rebuilding its stable executable: pid=$($candidate.Id)."
        }
    }
}

$repositoryRoot = Get-RepositoryRoot
$projectInfo = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repositoryRoot
$projectRoot = $projectInfo.Root
$projectExecutableName = $projectInfo.ExecutableName
$bDefaultProject = Test-DefaultProject -ProjectRoot $projectRoot -RepositoryRoot $repositoryRoot
$bGameMode = $Target -in @("HGame", "HEngineTests", "HEngineDX12SceneSmoke", "HEngineRmlDX12Smoke")
$bSelfContainedSmoke = $Target -in @("HEngineDX12SceneSmoke", "HEngineRmlDX12Smoke")
if ($PortableEditor -and $Target -ne "HEngineEditor") {
    throw "-PortableEditor is only supported for HEngineEditor."
}
$modeName = if ($Target -eq "HEngineTests") {
    "tests"
}
elseif ($bGameMode) {
    "game"
}
else {
    "editor"
}
if ($PortableEditor) {
    $modeName = "editor-portable"
}
$codeGenHostOverride = if ($Target -eq "HEngineTests") {
    "Tests"
}
elseif ($bGameMode) {
    "DevGame"
}
else {
    "Editor"
}
$buildTestsValue = if ($Target -eq "HEngineTests") { "ON" } else { "OFF" }
if ($EnableTracy) {
    if ($Target -ne "HGame" -and $Target -ne "HEngineEditor") {
        throw "-EnableTracy is only supported for HGame and HEngineEditor."
    }
    $modeName = "${modeName}-tracy"
}
$configurationName = $Configuration.ToLowerInvariant()
$buildRelativePath = if ($bDefaultProject) {
    "out\agent\{0}-{1}" -f $modeName, $configurationName
}
else {
    "out\agent\projects\{0}\{1}-{2}" -f $projectInfo.CacheKey, $modeName, $configurationName
}
$buildRoot = Join-Path $repositoryRoot $buildRelativePath
$runtimeOutput = Join-Path $buildRoot "bin"
$generatedSourceRoot = Join-Path $buildRoot "GeneratedSource"
$logRoot = if ($bDefaultProject) {
    Join-Path $repositoryRoot "out\agent\logs"
}
else {
    Join-Path $repositoryRoot ("out\agent\projects\{0}\logs" -f $projectInfo.CacheKey)
}
$cachePath = Join-Path $buildRoot "CMakeCache.txt"
$tracyValue = if ($EnableTracy) { "ON" } else { "OFF" }
$portableEditorValue = if ($PortableEditor) { "ON" } else { "OFF" }
$bRunRequested = $Run -or $Wait -or $TimeoutSeconds -gt 0 -or
    $ExitAfterFrames -gt 0 -or $FixedDeltaSeconds -gt 0.0 -or
    -not [string]::IsNullOrWhiteSpace($Resolution) -or $CaptureFrame -gt 0 -or
    -not [string]::IsNullOrWhiteSpace($CapturePath) -or
    -not [string]::IsNullOrWhiteSpace($StartupScene) -or $NoCursorCapture -or $NoVSync
$bWaitRequested = $Wait -or $TimeoutSeconds -gt 0 -or $ExitAfterFrames -gt 0 -or $CaptureFrame -gt 0
if ($PortableEditor -and $bRunRequested) {
    throw "-PortableEditor builds must be run from an assembled portable layout."
}

if ([double]::IsNaN($FixedDeltaSeconds) -or [double]::IsInfinity($FixedDeltaSeconds) -or
    $FixedDeltaSeconds -lt 0.0 -or $FixedDeltaSeconds -gt 1.0) {
    throw "-FixedDeltaSeconds must be 0 (disabled) or in the range (0, 1]."
}
if ($NoCursorCapture -and $Target -ne "HGame") {
    throw "-NoCursorCapture is only supported for HGame."
}
if (-not [string]::IsNullOrWhiteSpace($Resolution)) {
    if ($Resolution -notmatch "^(?<width>[1-9][0-9]{0,3})[xX](?<height>[1-9][0-9]{0,3})$") {
        throw "-Resolution must use WIDTHxHEIGHT with dimensions in the range 1..8192."
    }
    $resolutionWidth = [int]$matches.width
    $resolutionHeight = [int]$matches.height
    if ($resolutionWidth -gt 8192 -or $resolutionHeight -gt 8192) {
        throw "-Resolution must use WIDTHxHEIGHT with dimensions in the range 1..8192."
    }
    $Resolution = "${resolutionWidth}x${resolutionHeight}"
}
if ($CaptureFrame -eq 0 -and -not [string]::IsNullOrWhiteSpace($CapturePath)) {
    throw "-CapturePath requires -CaptureFrame."
}
if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
    if ($Target -ne "HEngineEditor") {
        throw "-StartupScene is only supported for HEngineEditor."
    }
    $StartupScene = $StartupScene.Replace('\', '/')
    if ([System.IO.Path]::IsPathRooted($StartupScene) -or
        $StartupScene -cnotlike 'Assets/*' -or
        [System.IO.Path]::GetExtension($StartupScene) -cne '.he' -or
        @($StartupScene.Split('/')) -contains '..') {
        throw "-StartupScene requires a safe Assets-relative .he path."
    }
}
if ($CaptureFrame -gt 0) {
    if ($Target -ne "HGame" -and $Target -ne "HEngineEditor") {
        throw "Frame capture is only supported for HGame and HEngineEditor."
    }
    if ($ExitAfterFrames -gt 0 -and $CaptureFrame -gt $ExitAfterFrames) {
        throw "-CaptureFrame cannot be greater than -ExitAfterFrames."
    }
    if ($ExitAfterFrames -eq 0) {
        $ExitAfterFrames = $CaptureFrame
    }
    if ([string]::IsNullOrWhiteSpace($CapturePath)) {
        $captureName = "{0}-{1}-{2}.png" -f $modeName, $configurationName, (Get-Date -Format "yyyyMMdd-HHmmss-fff")
        $captureRoot = if ($bDefaultProject) {
            Join-Path $repositoryRoot "out\agent\captures"
        }
        else {
            Join-Path $repositoryRoot ("out\agent\projects\{0}\captures" -f $projectInfo.CacheKey)
        }
        $CapturePath = Join-Path $captureRoot $captureName
    }
    elseif (-not [System.IO.Path]::IsPathRooted($CapturePath)) {
        $CapturePath = Join-Path $repositoryRoot $CapturePath
    }
    $CapturePath = [System.IO.Path]::GetFullPath($CapturePath)
    if ([System.IO.Path]::GetExtension($CapturePath) -ine ".png") {
        throw "-CapturePath must use the .png extension."
    }
}
if ($bRunRequested -and $Target -ne "HGame" -and $Target -ne "HEngineEditor" -and
    -not $bSelfContainedSmoke) {
    throw "Runtime options are only supported for HGame and HEngineEditor; use Run-Test.ps1 for HEngineTests."
}
if ($bSelfContainedSmoke -and ($ExitAfterFrames -gt 0 -or $FixedDeltaSeconds -gt 0.0 -or
    -not [string]::IsNullOrWhiteSpace($Resolution) -or $CaptureFrame -gt 0 -or
    -not [string]::IsNullOrWhiteSpace($CapturePath) -or $NoCursorCapture -or $NoVSync -or
    $RenderValidation -or $RenderStats)) {
    throw "$Target owns its frame bounds and validation; only -Run, -Wait, and -TimeoutSeconds are supported."
}

New-Item -ItemType Directory -Force -Path $buildRoot, $runtimeOutput, $logRoot | Out-Null
if ($Target -eq "HGame") {
    Assert-StableArtifactNotRunning `
        -ArtifactPath (Join-Path $runtimeOutput "${projectExecutableName}.exe") `
        -ProcessName $projectExecutableName
}
elseif ($Target -eq "HEngineEditor") {
    Assert-StableArtifactNotRunning `
        -ArtifactPath (Join-Path $runtimeOutput "HEngineEditor.exe") `
        -ProcessName "HEngineEditor"
}
Import-Vs2022Environment
$cmakeCommand = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source

$executionLockPath = Join-Path $buildRoot ".agent-execution.lock"
$executionLock = $null
try {
try {
    $executionLock = [System.IO.File]::Open(
        $executionLockPath,
        [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
}
catch {
    throw "Another Agent build or owned run is using $buildRoot. Wait for it to finish."
}

if ($Target -eq "HGame") {
    Assert-StableArtifactNotRunning `
        -ArtifactPath (Join-Path $runtimeOutput "${projectExecutableName}.exe") `
        -ProcessName $projectExecutableName
}
elseif ($Target -eq "HEngineEditor") {
    Assert-StableArtifactNotRunning `
        -ArtifactPath (Join-Path $runtimeOutput "HEngineEditor.exe") `
        -ProcessName "HEngineEditor"
}

$buildLockPath = Join-Path $buildRoot ".agent-build.lock"
$buildLock = $null
try {
    try {
        $buildLock = [System.IO.File]::Open(
            $buildLockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another Agent build is using $buildRoot. Wait for it to finish instead of sharing the Ninja directory."
    }

    $devGameValue = if ($bGameMode) { "ON" } else { "OFF" }
    $normalizedRuntimeOutput = ([System.IO.Path]::GetFullPath($runtimeOutput)).Replace('\', '/')
    $normalizedProjectRoot = $projectRoot.Replace('\', '/')
    $bConfigureRequired = -not (Test-Path -LiteralPath $cachePath)
    if (-not $bConfigureRequired) {
        if (-not (Test-CMakeCacheValue -Path $cachePath -Name "CMAKE_GENERATOR" -ExpectedValue "Ninja")) {
            throw "The Agent build cache does not use Ninja: $buildRoot. Use a fresh Agent build directory."
        }
        $bConfigureRequired =
            -not (Test-CMakeCacheValue -Path $cachePath -Name "CMAKE_BUILD_TYPE" -ExpectedValue $Configuration) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_DEV_GAME" -ExpectedValue $devGameValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PORTABLE_EDITOR" -ExpectedValue $portableEditorValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "PACK_GAME" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_ENABLE_GM" -ExpectedValue "ON") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_TESTS" -ExpectedValue $buildTestsValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "CPP_CODE_GEN" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_CODEGEN_HOST_OVERRIDE" -ExpectedValue $codeGenHostOverride) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_EFFEKSEER_BUILD_DX12" -ExpectedValue "ON") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_ENABLE_TRACY" -ExpectedValue $tracyValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_ON_DEMAND" -ExpectedValue "ON") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_MEMORY_PROFILING" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_CALLSTACK_DEPTH" -ExpectedValue "0") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PROJECT_DIR" -ExpectedValue $normalizedProjectRoot) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_GENERATED_SOURCE_DIR" -ExpectedValue (($generatedSourceRoot.Replace('\', '/')))) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_RUNTIME_OUTPUT_DIR" -ExpectedValue $normalizedRuntimeOutput)
    }

    if ($bConfigureRequired) {
        $configureLog = Join-Path $logRoot ("configure-{0}-{1}.log" -f $modeName, $configurationName)
        Invoke-LoggedCommand -Label "configure $modeName $Configuration" -LogPath $configureLog -Command {
            & $cmakeCommand -S $repositoryRoot -B $buildRoot -G Ninja `
                "-DCMAKE_BUILD_TYPE=$Configuration" `
                "-DHE_BUILD_DEV_GAME=$devGameValue" `
                "-DHE_PORTABLE_EDITOR=$portableEditorValue" `
                -DHE_ENABLE_GM=ON `
                "-DHE_BUILD_TESTS=$buildTestsValue" `
                "-DHE_PROJECT_DIR=$projectRoot" `
                "-DHE_GENERATED_SOURCE_DIR=$generatedSourceRoot" `
                "-DHE_RUNTIME_OUTPUT_DIR=$runtimeOutput" `
                -DPACK_GAME=OFF `
                -DCPP_CODE_GEN=OFF `
                "-DHE_CODEGEN_HOST_OVERRIDE=$codeGenHostOverride" `
                -DHE_EFFEKSEER_BUILD_DX12=ON `
                "-DHE_ENABLE_TRACY=$tracyValue" `
                -DHE_TRACY_ON_DEMAND=ON `
                -DHE_TRACY_MEMORY_PROFILING=OFF `
                -DHE_TRACY_CALLSTACK_DEPTH=0
        }
    }

    $buildLog = Join-Path $logRoot ("build-{0}-{1}-{2}.log" -f $modeName, $configurationName, $Target)
    Invoke-LoggedCommand -Label "build $Target" -LogPath $buildLog -Command {
        & $cmakeCommand --build $buildRoot --target $Target --parallel $Parallel
    }
}
finally {
    if ($null -ne $buildLock) {
        $buildLock.Dispose()
    }
}

$artifact = switch ($Target) {
    "HGame" { Join-Path $runtimeOutput "${projectExecutableName}.exe" }
    "HEngineEditor" { Join-Path $runtimeOutput "HEngineEditor.exe" }
    "HEngineParser" { Join-Path $runtimeOutput "Programs\HEngineParser\HEngineParser.exe" }
    "HAssetImporter" { Join-Path $runtimeOutput "Programs\HAssetImporter\HAssetImporter.exe" }
    "HCooker" { Join-Path $runtimeOutput "Programs\HCooker\HCooker.exe" }
    "HShaderCompiler" { Join-Path $runtimeOutput "Programs\HShaderCompiler\HShaderCompiler.exe" }
    "HEngineTests" { Join-Path $runtimeOutput "HEngineTests.exe" }
    "HEngineDX12SceneSmoke" {
        Join-Path $runtimeOutput "Programs\HEngineDX12SceneSmoke\HEngineDX12SceneSmoke.exe"
    }
    "HEngineRmlDX12Smoke" {
        Join-Path $runtimeOutput "Programs\HEngineRmlDX12Smoke\HEngineRmlDX12Smoke.exe"
    }
}
if (-not (Test-Path -LiteralPath $artifact)) {
    throw "Build completed but the expected artifact was not found: $artifact"
}
Write-Host "artifact: $artifact"

if ($bRunRequested) {
    $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $PID
    $stdoutLog = Join-Path $logRoot ("run-{0}-{1}-{2}.stdout.log" -f $modeName, $configurationName, $runId)
    $stderrLog = Join-Path $logRoot ("run-{0}-{1}-{2}.stderr.log" -f $modeName, $configurationName, $runId)

	$runArguments = [System.Collections.Generic.List[string]]::new()
	if ($Target -in @("HGame", "HEngineEditor")) {
		$runArguments.Add("--project=`"$projectRoot`"")
	}
    if ($bWaitRequested) {
        $sessionRoot = if ($bDefaultProject) {
            Join-Path $repositoryRoot "out\agent\sessions"
        }
        else {
            Join-Path $repositoryRoot ("out\agent\projects\{0}\sessions" -f $projectInfo.CacheKey)
        }
        $savedRoot = Join-Path $sessionRoot "$runId\Saved"
        New-Item -ItemType Directory -Force -Path $savedRoot | Out-Null
        $runArguments.Add("--saved-root=`"$savedRoot`"")
    }
    if ($ExitAfterFrames -gt 0) {
        $runArguments.Add("--exit-after-frames=$ExitAfterFrames")
    }
    if ($FixedDeltaSeconds -gt 0.0) {
        $fixedDeltaText = $FixedDeltaSeconds.ToString("R", [System.Globalization.CultureInfo]::InvariantCulture)
        $runArguments.Add("--fixed-delta=$fixedDeltaText")
    }
    if (-not [string]::IsNullOrWhiteSpace($Resolution)) {
        $runArguments.Add("--resolution=$Resolution")
    }
    if ($CaptureFrame -gt 0) {
        $runArguments.Add("--capture-frame=$CaptureFrame")
        $runArguments.Add("--capture-path=`"$CapturePath`"")
    }
    if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
        $runArguments.Add("--startup-scene=$StartupScene")
    }
    if ($NoCursorCapture) {
        $runArguments.Add("--no-cursor-capture")
    }
    if ($NoVSync -or $ExitAfterFrames -gt 0) {
        $runArguments.Add("--no-vsync")
    }
    if ($RenderValidation) {
        $runArguments.Add("--render-validation")
    }
    if ($RenderStats) {
        $runArguments.Add("--render-stats")
    }
    if (-not [string]::IsNullOrWhiteSpace($GraphicsApi)) {
        $runArguments.Add("--graphics-api=$($GraphicsApi.ToLowerInvariant())")
    }
    if ($StrictGraphicsApi) {
        $runArguments.Add("--strict-graphics-api")
    }
    if ($UseWarp) {
        $runArguments.Add("--warp")
    }

    $startParameters = @{
        FilePath = $artifact
        WorkingDirectory = $projectRoot
        PassThru = $true
        RedirectStandardOutput = $stdoutLog
        RedirectStandardError = $stderrLog
    }
    if ($runArguments.Count -gt 0) {
        $startParameters.ArgumentList = $runArguments.ToArray()
    }
    $process = $null
    $processJob = $null
    $bLeaveProcessRunning = $false
    try {
        if ($bWaitRequested) {
            $processJob = New-AgentProcessJob
        }
        $process = Start-Process @startParameters
        $null = $process.Handle
        if ($null -ne $processJob) {
            Add-AgentProcessToJob -Job $processJob -Process $process
        }
        $runStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        Write-Host ("run {0}: STARTED pid={1}; stdout: {2}; stderr: {3}" -f $Target, $process.Id, $stdoutLog, $stderrLog)

        $effectiveTimeoutSeconds = $TimeoutSeconds
        if ($effectiveTimeoutSeconds -eq 0 -and $ExitAfterFrames -gt 0) {
            $effectiveTimeoutSeconds = [Math]::Max(60, [Math]::Ceiling($ExitAfterFrames / 10.0))
        }
        $totalTimeoutMilliseconds = if ($effectiveTimeoutSeconds -gt 0) {
            $effectiveTimeoutSeconds * 1000.0
        } else { 0.0 }

        if ($Target -eq "HGame") {
            $readyTimeoutMilliseconds = 30000.0
            if ($totalTimeoutMilliseconds -gt 0) {
                $readyTimeoutMilliseconds = [Math]::Min($readyTimeoutMilliseconds, $totalTimeoutMilliseconds)
            }
            $bReady = $false
            while ($runStopwatch.Elapsed.TotalMilliseconds -lt $readyTimeoutMilliseconds) {
                if (Test-Path -LiteralPath $stdoutLog) {
                    $bReady = [bool](Select-String -LiteralPath $stdoutLog -SimpleMatch "[Game] Ready" -Quiet)
                    if ($bReady) {
                        break
                    }
                }
                $process.Refresh()
                if ($process.HasExited) {
                    break
                }
                Start-Sleep -Milliseconds 100
            }

            if (-not $bReady -and (Test-Path -LiteralPath $stdoutLog)) {
                $bReady = [bool](Select-String -LiteralPath $stdoutLog -SimpleMatch "[Game] Ready" -Quiet)
            }

            if (-not $bReady) {
                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-AgentOwnedProcess -Process $process -Description $Target
                }
                Write-Host "run HGame: FAIL (ready marker not observed)"
                Get-Content -LiteralPath $stdoutLog -Tail 50 -ErrorAction SilentlyContinue
                Get-Content -LiteralPath $stderrLog -Tail 50 -ErrorAction SilentlyContinue
                throw "HGame did not emit [Game] Ready before the run deadline."
            }
            Write-Host "run HGame: READY"
        }

        if ($bWaitRequested) {
            if ($totalTimeoutMilliseconds -gt 0) {
                $remainingMilliseconds = [Math]::Max(
                    0,
                    [Math]::Floor($totalTimeoutMilliseconds - $runStopwatch.Elapsed.TotalMilliseconds))
                $process.Refresh()
                $bExited = $process.HasExited -or $process.WaitForExit([int]$remainingMilliseconds)
            }
            else {
                $process.WaitForExit()
                $bExited = $true
            }

            if (-not $bExited) {
                Stop-AgentOwnedProcess -Process $process -Description $Target
                Get-Content -LiteralPath $stdoutLog -Tail 50 -ErrorAction SilentlyContinue
                Get-Content -LiteralPath $stderrLog -Tail 50 -ErrorAction SilentlyContinue
                throw "run ${Target}: TIMEOUT after $effectiveTimeoutSeconds seconds."
            }

            $process.WaitForExit()
            $process.Refresh()
            if ($process.ExitCode -ne 0) {
                Write-Host "run ${Target}: FAIL exit=$($process.ExitCode)"
                Get-Content -LiteralPath $stdoutLog -Tail 50 -ErrorAction SilentlyContinue
                Get-Content -LiteralPath $stderrLog -Tail 50 -ErrorAction SilentlyContinue
                exit $process.ExitCode
            }
            Write-Host "run ${Target}: PASS exit=0"
            if ($CaptureFrame -gt 0) {
                if (-not (Test-Path -LiteralPath $CapturePath)) {
                    throw "Runtime exited successfully but the requested capture was not found: $CapturePath"
                }
                Write-Host "capture: $CapturePath"
            }
        }
        else {
            $bLeaveProcessRunning = $true
        }
    }
    finally {
        try {
            if ($null -ne $process -and -not $bLeaveProcessRunning) {
                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-AgentOwnedProcess -Process $process -Description $Target
                }
            }
        }
        finally {
            if ($null -ne $processJob) {
                Close-AgentProcessJob -Job $processJob
            }
        }
    }
}
}
finally {
    if ($null -ne $executionLock) {
        $executionLock.Dispose()
    }
}
