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
        "HEngineFsrSmoke",
        "HEngineRmlDX12Smoke")]
    [string]$Target = "HGame",

    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet('', 'x64', 'arm64')][string]$Architecture = '',

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
    [string]$StartupViewportMode = "",
    [string]$InputReplayPath = "",

    [switch]$NoCursorCapture,

    [switch]$NoVSync,

    [switch]$RenderValidation,

    [switch]$RenderStats,

    [ValidateSet("", "Forward", "Deferred")]
    [string]$RenderPath = "",

    [string]$RenderStatsPath = "",

    [string]$RenderSettingsPath = "",

    [ValidateRange(0, 10000000)]
    [long]$PerformanceWarmupFrames = 30,

    [ValidateSet("", "DX12", "Vulkan")]
    [string]$GraphicsApi = "",

    [ValidateSet("All", "DX12", "Vulkan")]
    [string]$RenderBackends = "All",
    [ValidateSet("Full", "Minimal")]
    [string]$RuntimeFeatures = "Full",
    [ValidateSet("Physics2D", "Physics3D", "Navigation", "Svg", "Lottie")]
    [string[]]$DisableFeature = @(),

    [switch]$StrictGraphicsApi,

    [switch]$UseWarp,

    [switch]$EnableTracy,

    [switch]$PortableEditor,
    [switch]$PortablePlayer,
    [switch]$IncludeRenderSmokes,

    [string]$Project = "",

    # Explicit validation profiles share no state with the default/VS builds.
    [ValidatePattern('^$|^[a-z0-9][a-z0-9-]{0,47}$')]
    [string]$BuildVariant = "",
    [ValidatePattern('^$|^[a-z0-9][a-z0-9-]{0,47}$')]
    [string]$ArtifactVariant = "",
    [string]$PluginConfig = "",

    [switch]$ShowBuildOutput
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Toolchain.ps1")
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$BuildVariant = Get-HeArchitectureVariant $BuildVariant $Architecture
if ($UseWarp -and $GraphicsApi -eq "Vulkan") {
    throw "-UseWarp is valid only with -GraphicsApi DX12."
}
. (Join-Path $PSScriptRoot "Process.ps1")
. (Join-Path $PSScriptRoot "Rendering-Validation.ps1")

function Get-RepositoryRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

. (Join-Path $PSScriptRoot "Project.ps1")

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
$bGameMode = $Target -in @("HGame", "HEngineTests", "HEngineDX12SceneSmoke", "HEngineRmlDX12Smoke", "HEngineFsrSmoke")
$bSelfContainedSmoke = $Target -in @("HEngineDX12SceneSmoke", "HEngineRmlDX12Smoke", "HEngineFsrSmoke")
if ($PortableEditor -and $Target -ne "HEngineEditor") {
    throw "-PortableEditor is only supported for HEngineEditor."
}
if ($PortablePlayer -and ($Target -ne 'HGame' -or $PortableEditor)) {
    throw '-PortablePlayer is only supported for the standalone HGame host.'
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
if ($PortablePlayer) { $modeName = 'game-portable' }
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
if ($RenderBackends -ne "All" -and !$BuildVariant) { $BuildVariant = $RenderBackends.ToLowerInvariant() }
if ($RuntimeFeatures -eq 'Minimal' -or $DisableFeature.Count -gt 0) {
    $featureSuffix = if ($RuntimeFeatures -eq 'Minimal') { 'minimal' } else { 'no-' + (($DisableFeature | Sort-Object) -join '-').ToLowerInvariant() }
    if (!$BuildVariant) { $BuildVariant = $featureSuffix }
    elseif ($BuildVariant -notlike "*$featureSuffix*") { $BuildVariant += "-$featureSuffix" }
}
if ($BuildVariant) { $modeName = "${modeName}-${BuildVariant}" }
$buildRelativePath = if ($bDefaultProject) {
    "out\agent\{0}-{1}" -f $modeName, $configurationName
}
else {
    "out\agent\projects\{0}\{1}-{2}" -f $projectInfo.CacheKey, $modeName, $configurationName
}
$buildRoot = Join-Path $repositoryRoot $buildRelativePath
$runtimeOutput = Join-Path $buildRoot "bin"
if ($ArtifactVariant) { $runtimeOutput = Join-Path $runtimeOutput $ArtifactVariant }
if ($PluginConfig) { $PluginConfig = (Resolve-Path -LiteralPath $PluginConfig -ErrorAction Stop).Path.Replace('\', '/') }
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
$portablePlayerValue = if ($PortablePlayer) { "ON" } else { "OFF" }
$bRunRequested = $Run -or $Wait -or $TimeoutSeconds -gt 0 -or
    $ExitAfterFrames -gt 0 -or $FixedDeltaSeconds -gt 0.0 -or
    -not [string]::IsNullOrWhiteSpace($Resolution) -or $CaptureFrame -gt 0 -or
    -not [string]::IsNullOrWhiteSpace($CapturePath) -or
    -not [string]::IsNullOrWhiteSpace($StartupScene) -or
    -not [string]::IsNullOrWhiteSpace($InputReplayPath) -or $NoCursorCapture -or $NoVSync
$bWaitRequested = $Wait -or $TimeoutSeconds -gt 0 -or $ExitAfterFrames -gt 0 -or $CaptureFrame -gt 0
if (($PortableEditor -or $PortablePlayer) -and $bRunRequested) {
    throw "Portable builds must be run from an assembled portable layout."
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
    if ($Target -notin @("HEngineEditor", "HGame")) {
        throw "-StartupScene is only supported for HEngineEditor and development HGame."
    }
    $StartupScene = $StartupScene.Replace('\', '/')
    if ([System.IO.Path]::IsPathRooted($StartupScene) -or
        $StartupScene -cnotlike 'Assets/*' -or
        [System.IO.Path]::GetExtension($StartupScene) -cne '.he' -or
        @($StartupScene.Split('/')) -contains '..') {
        throw "-StartupScene requires a safe Assets-relative .he path."
    }
}
if (-not [string]::IsNullOrWhiteSpace($InputReplayPath)) {
    if ($Target -ne 'HGame' -or $ExitAfterFrames -le 0) {
        throw '-InputReplayPath requires a bounded HGame run.'
    }
    if (-not [System.IO.Path]::IsPathRooted($InputReplayPath)) {
        $InputReplayPath = Join-Path $projectRoot $InputReplayPath
    }
    $InputReplayPath = [System.IO.Path]::GetFullPath($InputReplayPath)
    if ([System.IO.Path]::GetExtension($InputReplayPath).ToLowerInvariant() -ne '.json' -or
        -not (Test-Path -LiteralPath $InputReplayPath -PathType Leaf)) {
        throw '-InputReplayPath requires an existing JSON file.'
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
    $bStorageOwnCapture = [string]::IsNullOrWhiteSpace($CapturePath)
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
    if ([System.IO.Path]::GetExtension($CapturePath).ToLowerInvariant() -notin @('.png', '.pfm', '.raw')) {
        throw "-CapturePath must use .png (display), .pfm or .raw (scene-linear HDR)."
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
$toolchain = Get-HeVisualStudio -Architecture $Architecture
Import-HeToolchain -Toolchain $toolchain
Write-Host "toolchain: $($toolchain.Generator), $($toolchain.Toolset), MSVC $($toolchain.ToolsVersion)"
$cmakeCommand = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source

. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = $null
$buildEvidenceRun = $null
$storageEvidenceComplete = $true
$configureLog = $null
$buildLog = $null
$process = $null
$storageState = 'failed'
$executionLockPath = Join-Path $buildRoot ".agent-execution.lock"
$executionLock = $null
$cacheLease = $null
try {
$cacheLease = Open-HECacheLease -CachePath $buildRoot
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

$buildEvidenceDirectory = Join-Path $logRoot ('build-run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $buildEvidenceDirectory | Out-Null
$buildEvidenceRun = Start-HEStorageRun -Project $projectRoot -Workflow "build-$Target-$modeName-$configurationName" -Paths @($buildEvidenceDirectory) -Locks @($executionLockPath) -Outer
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

    $assetToolsValue = if ($Target -in @('HAssetImporter','HCooker','HEngineEditor')) { 'ON' } else { 'OFF' }
    $renderSmokesValue = if ($bSelfContainedSmoke -or $IncludeRenderSmokes) { 'ON' } else { 'OFF' }
    $featureValues = [ordered]@{}
    $featureNames = [ordered]@{ Physics2D='PHYSICS_2D'; Physics3D='PHYSICS_3D'; Navigation='NAVIGATION'; Svg='UI_SVG'; Lottie='UI_LOTTIE' }
    foreach ($feature in $featureNames.Keys) {
        $featureValues["HE_ENABLE_$($featureNames[$feature])"] = if ($RuntimeFeatures -eq 'Minimal' -or $feature -in $DisableFeature) { 'OFF' } else { 'ON' }
    }
    $featureArguments = @($featureValues.Keys | ForEach-Object { "-D$_=$($featureValues[$_])" })
    $dx12Value = if ($RenderBackends -eq "Vulkan") { "OFF" } else { "ON" }
    $vulkanValue = if ($RenderBackends -eq "DX12") { "OFF" } else { "ON" }
    $devGameValue = if ($bGameMode) { "ON" } else { "OFF" }
    $normalizedRuntimeOutput = ([System.IO.Path]::GetFullPath($runtimeOutput)).Replace('\', '/')
    $normalizedProjectRoot = $projectRoot.Replace('\', '/')
    $freshConfigure = @(Get-HeFreshConfigureArguments -BuildRoot $buildRoot -Compiler $toolchain.Compiler -Generator Ninja)
    # A terminated configure can leave a valid-looking cache before Ninja writes
    # its build graph. Reconfigure that cache instead of failing at build time.
    $bConfigureRequired = $freshConfigure.Count -gt 0 -or
        -not (Test-Path -LiteralPath $cachePath) -or
        -not (Test-Path -LiteralPath (Join-Path $buildRoot "build.ninja"))
    if (-not $bConfigureRequired) {
        $bConfigureRequired =
            -not (Test-CMakeCacheValue -Path $cachePath -Name "CMAKE_BUILD_TYPE" -ExpectedValue $Configuration) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_DEV_GAME" -ExpectedValue $devGameValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PORTABLE_EDITOR" -ExpectedValue $portableEditorValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PORTABLE_PLAYER" -ExpectedValue $portablePlayerValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "PACK_GAME" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_ENABLE_GM" -ExpectedValue "ON") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_ASSET_TOOLS" -ExpectedValue $assetToolsValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_RENDER_SMOKES" -ExpectedValue $renderSmokesValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_BUILD_TESTS" -ExpectedValue $buildTestsValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "CPP_CODE_GEN" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_CODEGEN_HOST_OVERRIDE" -ExpectedValue $codeGenHostOverride) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_RENDER_DX12" -ExpectedValue $dx12Value) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_RENDER_VULKAN" -ExpectedValue $vulkanValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_ENABLE_TRACY" -ExpectedValue $tracyValue) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_ON_DEMAND" -ExpectedValue "ON") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_MEMORY_PROFILING" -ExpectedValue "OFF") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_TRACY_CALLSTACK_DEPTH" -ExpectedValue "0") -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PROJECT_DIR" -ExpectedValue $normalizedProjectRoot) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_PLUGIN_CONFIG" -ExpectedValue $PluginConfig) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_GENERATED_SOURCE_DIR" -ExpectedValue (($generatedSourceRoot.Replace('\', '/')))) -or
            -not (Test-CMakeCacheValue -Path $cachePath -Name "HE_RUNTIME_OUTPUT_DIR" -ExpectedValue $normalizedRuntimeOutput)
    }

    foreach ($feature in $featureValues.Keys) {
        if (!(Test-CMakeCacheValue -Path $cachePath -Name $feature -ExpectedValue $featureValues[$feature])) { $bConfigureRequired = $true }
    }
    if ($bConfigureRequired) {
        $configureLog = Join-Path $logRoot ("configure-{0}-{1}.log" -f $modeName, $configurationName)
        Invoke-LoggedCommand -Label "configure $modeName $Configuration" -LogPath $configureLog -Command {
            & $cmakeCommand @freshConfigure @featureArguments -S $repositoryRoot -B $buildRoot -G Ninja `
                "-DCMAKE_C_COMPILER=$($toolchain.Compiler)" `
                "-DCMAKE_CXX_COMPILER=$($toolchain.Compiler)" `
                "-DCMAKE_BUILD_TYPE=$Configuration" `
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
                "-DHE_BUILD_DEV_GAME=$devGameValue" `
                "-DHE_PORTABLE_EDITOR=$portableEditorValue" `
                "-DHE_PORTABLE_PLAYER=$portablePlayerValue" `
                -DHE_ENABLE_GM=ON `
                "-DHE_BUILD_TESTS=$buildTestsValue" `
                "-DHE_BUILD_ASSET_TOOLS=$assetToolsValue" `
                "-DHE_BUILD_RENDER_SMOKES=$renderSmokesValue" `
                "-DHE_CODEGEN_LIBCLANG_ROOT=$env:HE_CODEGEN_LIBCLANG_ROOT" `
                "-DHE_CODEGEN_PARSER_EXECUTABLE=$env:HE_CODEGEN_PARSER_EXECUTABLE" `
                "-DHE_PROJECT_DIR=$projectRoot" `
                "-DHE_PLUGIN_CONFIG=$PluginConfig" `
                "-DHE_GENERATED_SOURCE_DIR=$generatedSourceRoot" `
                "-DHE_RUNTIME_OUTPUT_DIR=$runtimeOutput" `
                -DPACK_GAME=OFF `
                -DCPP_CODE_GEN=OFF `
                "-DHE_CODEGEN_HOST_OVERRIDE=$codeGenHostOverride" `
                "-DHE_RENDER_DX12=$dx12Value" `
                "-DHE_RENDER_VULKAN=$vulkanValue" `
                "-DHE_SHADER_SPIRV=$vulkanValue" `
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
    "HEngineFsrSmoke" {
        Join-Path $runtimeOutput "Programs\HEngineFsrSmoke\HEngineFsrSmoke.exe"
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
        $storagePaths = @((Split-Path -Parent $savedRoot), $stdoutLog, $stderrLog)
        if ($bStorageOwnCapture) { $storagePaths += $CapturePath }
        $storageRun = Start-HEStorageRun -Project $projectRoot -Workflow "$Target-$modeName-$configurationName" -Paths $storagePaths -Locks @($executionLockPath)
        if ($storageRun) {
            try {
                # VS environment import can change module discovery in Windows
                # PowerShell. Receipt hashing must not depend on cmdlet autoload.
                $artifactStream = [IO.File]::OpenRead($artifact)
                $artifactHasher = [Security.Cryptography.SHA256]::Create()
                try {
                    $artifactHash = [BitConverter]::ToString($artifactHasher.ComputeHash($artifactStream)).Replace('-', '')
                }
                finally {
                    $artifactHasher.Dispose()
                    $artifactStream.Dispose()
                }
                [ordered]@{artifact=$artifact;sha256=$artifactHash;target=$Target;configuration=$Configuration;scene=$StartupScene;storageId=$storageRun.Id} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path (Split-Path -Parent $savedRoot) 'run.json') -Encoding UTF8
            }
            catch { $storageEvidenceComplete = $false; Write-Warning "storage runtime receipt failed: $($_.Exception.Message)" }
        }
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
    if (-not [string]::IsNullOrWhiteSpace($InputReplayPath)) {
        $runArguments.Add("--input-replay=`"$InputReplayPath`"")
    }
    if (-not [string]::IsNullOrWhiteSpace($StartupViewportMode)) {
        $runArguments.Add("--viewport-mode=$StartupViewportMode")
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
    if (-not [string]::IsNullOrWhiteSpace($RenderPath)) {
        $runArguments.Add("--render-path=$($RenderPath.ToLowerInvariant())")
    }
    if (-not [string]::IsNullOrWhiteSpace($RenderSettingsPath)) {
        $RenderSettingsPath = [System.IO.Path]::GetFullPath($RenderSettingsPath)
        if (-not (Test-Path -LiteralPath $RenderSettingsPath -PathType Leaf)) {
            throw "Render settings JSON was not found: $RenderSettingsPath"
        }
        $runArguments.Add("--render-settings=`"$RenderSettingsPath`"")
    }
    if (-not [string]::IsNullOrWhiteSpace($RenderStatsPath)) {
        $RenderStatsPath = [System.IO.Path]::GetFullPath($RenderStatsPath)
        $runArguments.Add("--render-stats-path=`"$RenderStatsPath`"")
    }
    $runArguments.Add("--performance-warmup-frames=$PerformanceWarmupFrames")
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
            # Large benchmark scenes legitimately need longer than 30 seconds
            # to import/upload. The caller's owned-run deadline remains binding.
            $readyTimeoutMilliseconds = 300000.0
            if ($totalTimeoutMilliseconds -gt 0) {
                $readyTimeoutMilliseconds = $totalTimeoutMilliseconds
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
            if ($RenderValidation) {
                Assert-RenderingValidationLogs -Paths @($stdoutLog, $stderrLog)
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
$storageState = $(if ($storageEvidenceComplete) { 'success' } else { 'unknown' })
}
finally {
    if ($buildEvidenceRun) {
        try {
            foreach ($evidenceLog in @($configureLog, $buildLog)) {
                if ($evidenceLog -and (Test-Path -LiteralPath $evidenceLog)) { Copy-Item -LiteralPath $evidenceLog -Destination $buildEvidenceDirectory }
            }
            [ordered]@{target=$Target;configuration=$Configuration;project=$projectRoot;buildRoot=$buildRoot;state=$storageState;artifact=$artifact} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildEvidenceDirectory 'result.json') -Encoding UTF8
        }
        catch { $storageState = 'unknown'; Write-Warning "storage build evidence snapshot failed: $($_.Exception.Message)" }
    }
    if ($process) {
        try { $process.Refresh(); if (-not $process.HasExited) { $storageState = 'unknown' } }
        catch { $storageState = 'unknown' }
    }
    if ($null -ne $executionLock) {
        $executionLock.Dispose()
    }
    if ($cacheLease) { $cacheLease.Dispose() }
    if ($storageRun) { Complete-HEStorageRun -Run $storageRun -State $storageState }
    if ($buildEvidenceRun) { Complete-HEStorageRun -Run $buildEvidenceRun -State $storageState }
    else { Invoke-HEStorageAuto }
}
