param(
    [Parameter(Position = 0)]
    [string]$Scope = "game",

    [Parameter(Position = 1)]
    [string]$Action = "",

    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [switch]$AcceptAndroidLicenses,

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 60,

    [ValidateRange(1, 10000000)]
    [long]$Frames = 120,

    [double]$FixedDeltaSeconds = 0.0166667,

    [string]$Resolution = "1280x720",

    [switch]$UseProjectDisplaySettings,

    [string]$CapturePath = "",

    [string]$StartupScene = "",
    [string]$StartupViewportMode = "",
    [string]$InputReplayPath = "",

    [string]$Method = "",

    [string]$ParamsJson = "{}",

    [string]$RequestFile = "",

    [string]$Source = "",
    [string]$Destination = "",
    [ValidateSet("none", "visual", "runtime")]
    [string]$Verify = "none",
    [string]$BlenderExe = "",
    [string]$ImporterExe = "",
    [switch]$InitializeIds,
    [string]$Report = "",
    [string]$ObjectId = "",
    [string]$ErrorCode = "",

    [string]$GMCommand = "",

    [string]$GMArgumentsJson = "{}",

    [switch]$GMConfirm,

    [string]$AgentCaptureName = "",

    [string]$AgentHeldKeys = "",

    [double]$AgentClickX = [double]::NaN,

    [double]$AgentClickY = [double]::NaN,

    [ValidateSet("left", "right")]
    [string]$AgentClickButton = "left",

    [ValidateRange(1, 120)]
    [int]$AgentStepFrames = 1,

    [switch]$EnableTracy,

    [switch]$RenderValidation,

    [switch]$RenderStats,

    [ValidateSet("", "Forward", "Deferred")]
    [string]$RenderPath = "",

    [string]$RenderStatsPath = "",

    [string]$RenderSettingsPath = "",

    [ValidatePattern('^$|^[0-9a-fA-F]{64}$')]
    [string]$ExistingBuildSha256 = "",

    [ValidateRange(0, 10000000)]
    [long]$PerformanceWarmupFrames = 30,

    [ValidateSet("", "DX12", "Vulkan")]
    [string]$GraphicsApi = "",

    [switch]$StrictGraphicsApi,

    [switch]$UseWarp,

    [ValidateScript({
        if ([string]::IsNullOrEmpty($_) -or $_ -match '^[0-9a-fA-F]{32}$') {
            return $true
        }
        throw "-AgentOwnerTag must be empty or exactly 32 hexadecimal characters."
    })]
    [string]$AgentOwnerTag = "",

    [switch]$WhatIf,
    [string]$Category = "",
    [string]$CachePath = "",
    [int]$KeepSuccess = -1,
    [int]$FailureDays = -1,
    [long]$BudgetBytes = -1,

    [string]$Project = "",
    [string]$Name = "",
    [string]$Template = 'Blank',
    [string]$GameConfig = "",
    [string]$PluginConfig = "",
    [string]$BuildProfile = "windows",
    [string]$DevelopmentTemplate = "",

    [ValidateSet('Editor','DevGame','Tests','Package','Parser','SDK','Source')]
    [string]$Profile = 'Editor',
    [switch]$Offline,

    [ValidateSet("Editor", "DevGame", "Tests", "Package")]
    [string]$CodeGenHost = "Editor",

    [switch]$Json,

    [switch]$Quiet,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArguments
)

$ErrorActionPreference = "Stop"
if ($UseProjectDisplaySettings -and $PSBoundParameters.ContainsKey('Resolution')) {
    throw '-UseProjectDisplaySettings cannot be combined with an explicit -Resolution.'
}
if (-not [string]::IsNullOrWhiteSpace($RequestFile)) {
    $requestInfo = Get-Item -LiteralPath $RequestFile
    if ($requestInfo.Length -gt 262144) { throw "Scene request file exceeds 256 KiB." }
    $ParamsJson = [System.IO.File]::ReadAllText($requestInfo.FullName, [System.Text.UTF8Encoding]::new($false, $true))
    $null = $ParamsJson | ConvertFrom-Json
    if ($Scope -eq "content") {
        $contentRequest = $ParamsJson | ConvertFrom-Json
        foreach ($property in $contentRequest.PSObject.Properties) {
            switch ($property.Name) {
                'source' { $Source = [string]$property.Value }
                'project' { $Project = [string]$property.Value }
                'verify' { $Verify = [string]$property.Value }
                'blender' { $BlenderExe = [string]$property.Value }
                'initializeIds' { $InitializeIds = [bool]$property.Value }
                'timeoutSeconds' { $TimeoutSeconds = [int]$property.Value }
                default { throw "Unknown content request property: $($property.Name)" }
            }
        }
        if ($Verify -notin @('none','visual','runtime') -or $TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 3600) { throw 'Invalid content verification mode or timeout.' }
    }
}
if ($Scope -eq "scene") {
    $Method = "scene.$Action"
    $Scope = "agent"
    $Action = "call"
}
. (Join-Path $PSScriptRoot "Toolchain.ps1")

foreach ($remainingArgument in @($RemainingArguments)) {
    if ($null -eq $remainingArgument) {
        continue
    }
    switch ($remainingArgument.ToLowerInvariant()) {
        "--json" { $Json = $true }
        "--quiet" { $Quiet = $true }
        default { throw "Unknown argument '$remainingArgument'." }
    }
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

function Get-ProjectBuildRoot {
    param(
        [ValidateSet("game", "editor", "tests")]
        [string]$Mode,
        [string]$ConfigurationName
    )

    $buildMode = if ($EnableTracy) { "${Mode}-tracy" } else { $Mode }
    if ($script:bDefaultProject) {
        return Join-Path $script:repositoryRoot ("out\agent\{0}-{1}" -f $buildMode, $ConfigurationName)
    }
    return Join-Path $script:repositoryRoot `
        ("out\agent\projects\{0}\{1}-{2}" -f $script:projectInfo.CacheKey, $buildMode, $ConfigurationName)
}

function Show-Help {
    $configurationName = $Configuration.ToLowerInvariant()
    $gameArtifact = Join-Path (Get-ProjectBuildRoot -Mode "game" -ConfigurationName $configurationName) `
        ("bin\{0}.exe" -f $script:projectInfo.ExecutableName)
    $editorArtifact = Join-Path (Get-ProjectBuildRoot -Mode "editor" -ConfigurationName $configurationName) `
        "bin\HEngineEditor.exe"
    Write-Host @"
HEngine command line workflow

  hectl project manager         Open the standalone project manager (no build required)
  hectl project create -Project D:\Games\NewGame -Name "New Game"
                                Add -Template FirstPerson or -Template TopDown for playable starter projects
  hectl project templates       List available project templates and controls (-Json supported)
  hectl project use -Project D:\Games\NewGame
                                Remember the default for this engine workspace
  hectl project current -Json    Explain project, engine and artifact locations
  hectl project list            List recently registered projects
  hectl project reset           Restore the checked-in workspace default
  hectl                         Build optimized HGame Dev (RelWithDebInfo, PACK_GAME=OFF)
  hectl doctor                  Read compact toolchain/artifact/session state
  hectl lighting bake|check -Project D:\Games\MyGame [-Source Config/LightingBake.json]
                                Bake/check portable offline lightmaps and probes
  hectl deps check|install -Profile Source|Editor|DevGame|Tests|Package|Parser|SDK [-Offline] [-WhatIf]
  hectl capabilities [-Json]                 Binary SDK platform/feature contract (no project required)
  hectl publish check|build -BuildProfile windows|android|web|wechat [-Project <path>]
  hectl clean status            Read-only logical-byte inventory and retention report
  hectl clean caches            Compact read-only build-cache sizes and last writes
  hectl clean maximize          Reclaim verified idle output, preserving pins and recovery data
  hectl clean maximize -WhatIf  Preview the same out-only cleanup without filesystem writes
  hectl clean stale             Recycle only owned completed expired artifacts
  hectl clean stale -WhatIf     Preview without filesystem mutations
  hectl clean preview --json    Structured candidates and protection reasons
    -Category evidence|package-intermediate|package-staging|wechat-backup|wechat-staging
    -KeepSuccess 5 -FailureDays 7 -BudgetBytes 5368709120 -Report PATH
  hectl clean cache -CachePath out/agent/game-tracy-debug -WhatIf
    Explicit entire isolated cache retirement; never automatic; VS caches are report-only.
    Policy: Tools/Agent/storage-policy.json; caches/SDK/current exports protected
  hectl game build              Build HGame Dev
  hectl game check              Agent-owned bounded game run
  hectl game check -GraphicsApi Vulkan -StrictGraphicsApi
                                Require a real Vulkan bounded run (no fallback)
  hectl game check -GraphicsApi DX12 -UseWarp
                                Require the DX12 WARP compatibility path
  hectl game run                Alias of game check
  hectl game capture            Bounded game run with a deterministic PNG
  hectl game check -InputReplayPath C:\Games\MyGame\Tools\walk.json
                                Bounded Dev Game run with segment-based held-key replay
  hectl game open               Manual game run; streams build output
  hectl editor build            Build HEngineEditor in addition to HGame as needed
  hectl editor check            Agent-owned bounded editor run
  hectl editor check -GraphicsApi Vulkan -StrictGraphicsApi
                                Require a real Vulkan Editor bounded run
  hectl editor run              Alias of editor check
  hectl editor capture          Bounded editor run with a main-window UI PNG
  hectl editor capture -RenderValidation  Enable DX12/NVRHI validation for this run
  hectl editor capture -RenderStats       Print measured frame/render statistics
  hectl editor open             Manual editor run; streams build output
  hectl agent start             Start a paused Dev Game inspection session
  hectl agent start -BuildProfile windows
                                Binary SDK: compile C# and run the installed Dev Player
  hectl agent start -StartupScene Assets/Scenes/MainGame.he
                                Start directly in a deterministic gameplay scene
  hectl agent start -EnableTracy  Use an isolated Tracy-enabled HGame build
  hectl agent start -AgentOwnerTag <32-hex>  Tag a caller-owned session
  hectl agent status            Read compact structured session state
  hectl agent discover          Discover implemented runtime commands
  hectl agent gm-discover       Discover typed Engine and Project GM commands
  hectl agent gm-execute        Execute -GMCommand with -GMArgumentsJson
  hectl agent world             Read a bounded world summary
  hectl agent pause|resume      Control runtime simulation
  hectl agent step              Advance -AgentStepFrames deterministic frames
  hectl agent keys              List canonical virtual keyboard keys
  hectl agent input             Replace held keys from -AgentHeldKeys w,a
  hectl agent clear-input       Release every virtual key
  hectl agent click             Click at -AgentClickX/-AgentClickY
  hectl agent capture           Capture a live frame after the command settles
  hectl agent call -Method ...  Invoke a discovered command with -ParamsJson
  hectl scene open|describe|query|plan|apply|diff|validate|save|undo|redo|preview -RequestFile request.json
                               Shared authoring service in an Agent-owned session
  hectl agent stop              Gracefully close the Agent-owned game
  hectl test list               List headless engine tests
  hectl test <name>             Run exactly one headless engine test
  hectl wechat doctor          Inspect local WeChat host tools and configuration
  hectl scripting setup       Install locked .NET Wasm and JS probe build tools
  hectl scripting probe-pc    Build and run the native CoreCLR compatibility probe
  hectl scripting probe-web   Build the browser C# compatibility probe
  hectl scripting probe-wechat Build the WXWebAssembly C# compatibility probe
  hectl scripting matrix-pc   Build/run isolated PC plugin configurations and inspect dependencies
  hectl scripting build-web   Build/export the real C# and C++ Web sample
  hectl scripting build-wechat Build/export the same C# and C++ sample for WXWebAssembly
  hectl scripting serve-web   Serve the exported Web game locally with Wasm MIME types
  hectl wechat setup           Download pinned Emscripten and sokol_gfx dependencies
  hectl wechat check           Build Wasm, export locally, run native/host contracts
  hectl wechat export          Build and export the strict WeChat sample profile
  hectl codegen status          Report manifest freshness without generating
  hectl codegen check           Exit nonzero when reflection outputs are dirty
  hectl codegen run             Generate transactionally when inputs are dirty
  hectl codegen status --json   Emit the stable machine-readable result schema
  hectl codegen run --quiet     Suppress successful terminal output
  hectl android setup -AcceptAndroidLicenses  Install managed Android toolchain
  hectl android check|build     Check or package native Android Vulkan
  hectl package                 Cook/package Release game (PACK_GAME=ON)
  hectl package -GameConfig Config/Game.ScriptSample.json
                                Package an alternate game config into an isolated output

Project selection:
  -Project <directory|Project.heproject>
  default: $($script:projectInfo.Root)

Configuration defaults:
  game/editor/agent: RelWithDebInfo
  test/codegen/doctor: Debug
  override game/editor/agent with -Configuration Debug|RelWithDebInfo|Release
  Release development checks keep PACK_GAME=OFF; use package for shipping validation

Agent automation must use check/run, never open. The check/run commands own and
close every process they start. The open command and Win-Run*.bat launchers are
for manual observation.

Stable $Configuration executables for the selected project:
  $gameArtifact
  $editorArtifact
"@
}

function Get-CMakeCacheMode {
    param([string]$CachePath)

    if (-not (Test-Path -LiteralPath $CachePath)) {
        return $null
    }
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $CachePath) {
        if ($line -match "^(HE_BUILD_DEV_GAME|PACK_GAME|HE_ENABLE_GM|CMAKE_BUILD_TYPE|HE_ENABLE_TRACY|HE_PROJECT_DIR):[^=]+=(.*)$") {
            $values[$matches[1]] = $matches[2]
        }
    }
    return [ordered]@{
        configuration = $values.CMAKE_BUILD_TYPE
        devGame = $values.HE_BUILD_DEV_GAME
        packGame = $values.PACK_GAME
        gm = $values.HE_ENABLE_GM
        tracy = $values.HE_ENABLE_TRACY
        project = $values.HE_PROJECT_DIR
    }
}

function Test-ArtifactRunning {
    param(
        [string]$ArtifactPath,
        [string]$ProcessName
    )

    if (-not (Test-Path -LiteralPath $ArtifactPath)) {
        return $false
    }
    $expectedPath = [System.IO.Path]::GetFullPath($ArtifactPath)
    foreach ($candidate in Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        try {
            if ([string]::Equals(
                [System.IO.Path]::GetFullPath($candidate.Path),
                $expectedPath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
                return $true
            }
        }
        catch {
        }
    }
    return $false
}

function Show-Doctor {
    $configurationName = $Configuration.ToLowerInvariant()
    $gameRoot = Get-ProjectBuildRoot -Mode "game" -ConfigurationName $configurationName
    $editorRoot = Get-ProjectBuildRoot -Mode "editor" -ConfigurationName $configurationName
    $gameArtifact = Join-Path $gameRoot ("bin\{0}.exe" -f $script:projectInfo.ExecutableName)
    $editorArtifact = Join-Path $editorRoot "bin\HEngineEditor.exe"
	$packageOutputRoot = if ($script:bDefaultProject) {
		Join-Path $script:repositoryRoot "dist"
	}
	else {
		Join-Path $script:repositoryRoot ("dist\projects\{0}" -f $script:projectInfo.PackageKey)
	}
	$packageDirectory = Join-Path $packageOutputRoot $script:projectInfo.ExecutableName
    $activeSessionPath = if ($script:bDefaultProject) {
        Join-Path $script:repositoryRoot "out\agent\sessions\active.json"
    } else { Join-Path $gameRoot 'sessions/active.json' }
    $session = $null
    if (Test-Path -LiteralPath $activeSessionPath) {
        try {
            $owner = Get-Content -Raw -Encoding UTF8 -LiteralPath $activeSessionPath | ConvertFrom-Json
            $recordedProcess = Get-Process -Id ([int]$owner.pid) -ErrorAction SilentlyContinue
            $bProcessAlive = $false
            if ($null -ne $recordedProcess) {
                try {
                    $bProcessAlive = $recordedProcess.StartTime.ToUniversalTime().Ticks -eq
                        [long]$owner.processStartUtcTicks
                }
                catch {
                    $bProcessAlive = $false
                }
            }
            $session = [ordered]@{
                state = [string]$owner.state
                pid = [int]$owner.pid
                directory = [string]$owner.sessionDirectory
                processAlive = $bProcessAlive
            }
        }
        catch {
            $session = [ordered]@{ state = "invalid" }
        }
    }

    $cmake = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $selectedToolchain = $null
    $toolchainError = $null
    try { $selectedToolchain = Get-HeVisualStudio }
    catch { $toolchainError = $_.Exception.Message }
    $result = [ordered]@{
        project = [ordered]@{
            root = $script:projectInfo.Root
            descriptor = $script:projectInfo.DescriptorPath
            id = $script:projectInfo.ProjectId
            executable = $script:projectInfo.ExecutableName
            cacheKey = $script:projectInfo.CacheKey
        }
        workflow = [ordered]@{
            defaultTarget = "HGame"
            devGame = $true
            packGame = $false
            gm = $true
        }
        toolchain = [ordered]@{
            preference = "VS2026/v145, then VS2022/v143"
            selected = $selectedToolchain
            error = $toolchainError
            cmake = if ($null -ne $cmake) { $cmake.Source } else { $null }
            vswhere = Test-Path -LiteralPath (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
        }
        game = [ordered]@{
            artifact = $gameArtifact
            exists = Test-Path -LiteralPath $gameArtifact
            running = Test-ArtifactRunning -ArtifactPath $gameArtifact -ProcessName $script:projectInfo.ExecutableName
            cache = Get-CMakeCacheMode -CachePath (Join-Path $gameRoot "CMakeCache.txt")
        }
        editor = [ordered]@{
            artifact = $editorArtifact
            exists = Test-Path -LiteralPath $editorArtifact
            running = Test-ArtifactRunning -ArtifactPath $editorArtifact -ProcessName "HEngineEditor"
            cache = Get-CMakeCacheMode -CachePath (Join-Path $editorRoot "CMakeCache.txt")
        }
        agentSession = $session
        package = [ordered]@{
            directory = $packageDirectory
            executable = Join-Path $packageDirectory ("{0}.exe" -f $script:projectInfo.ExecutableName)
            exists = Test-Path -LiteralPath (Join-Path $packageDirectory ("{0}.exe" -f $script:projectInfo.ExecutableName))
        }
    }
    Write-Output ($result | ConvertTo-Json -Depth 8 -Compress)
}

function Invoke-Build {
    param(
        [string]$Target,
        [switch]$Run,
        [switch]$BoundedRun,
        [switch]$Capture,
        [switch]$ShowBuildOutput
    )

    $parameters = @{
        Target = $Target
        Configuration = $Configuration
        Project = $script:projectInfo.Root
        Parallel = $Parallel
        EnableTracy = $EnableTracy
        RenderValidation = $RenderValidation
        RenderStats = $RenderStats
        RenderPath = $RenderPath
        RenderStatsPath = $RenderStatsPath
        RenderSettingsPath = $RenderSettingsPath
        InputReplayPath = $InputReplayPath
        PerformanceWarmupFrames = $PerformanceWarmupFrames
        GraphicsApi = $GraphicsApi
        StrictGraphicsApi = $StrictGraphicsApi
        UseWarp = $UseWarp
    }
    if ($Run) {
        $parameters.Run = $true
        $parameters.StartupViewportMode = $StartupViewportMode
        if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
            $parameters.StartupScene = $StartupScene
        }
    }
    if ($ShowBuildOutput) {
        $parameters.ShowBuildOutput = $true
    }
    if ($BoundedRun) {
        $parameters.TimeoutSeconds = $TimeoutSeconds
        $parameters.ExitAfterFrames = $Frames
        $parameters.FixedDeltaSeconds = $FixedDeltaSeconds
        $parameters.NoVSync = $true
        if (-not $UseProjectDisplaySettings) { $parameters.Resolution = $Resolution }
        if ($Target -eq "HGame") {
            $parameters.NoCursorCapture = $true
        }
        if ($Capture) {
            $parameters.CaptureFrame = $Frames
            if (-not [string]::IsNullOrWhiteSpace($CapturePath)) {
                $parameters.CapturePath = $CapturePath
            }
        }
    }

    & $script:buildScript @parameters
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if ([double]::IsNaN($FixedDeltaSeconds) -or
    [double]::IsInfinity($FixedDeltaSeconds) -or
    $FixedDeltaSeconds -le 0.0 -or $FixedDeltaSeconds -gt 1.0) {
    throw "-FixedDeltaSeconds must be in the range (0, 1]."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ($Scope -ieq 'capabilities') {
    if ($Action) { throw 'Use hectl capabilities -Json without an action.' }
    $catalog = Get-Content -LiteralPath (Join-Path $repositoryRoot 'Tools/Build/Capabilities.json') -Raw -Encoding UTF8
    if ($Json) { Write-Output $catalog }
    else {
        $capabilities = $catalog | ConvertFrom-Json
        Write-Output $capabilities.scope
        foreach ($feature in $capabilities.features.PSObject.Properties) {
            $support = $feature.Value.support
            Write-Output "$($feature.Name): Windows=$($support.Windows), Android=$($support.Android), Web=$($support.Web), WeChat=$($support.WeChat)"
        }
    }
    exit 0
}
# Project management must remain available when there is no project, or a saved
# binding points to a missing/incompatible project.
if ($Scope -ieq 'project') {
    switch ($Action.ToLowerInvariant()) {
        'manager' { & (Join-Path $PSScriptRoot 'ProjectManager.ps1') -Project $Project; exit 0 }
        'templates' {
            if ($Json) { @(Get-HEProjectTemplates) | ConvertTo-Json }
            else { Get-HEProjectTemplates | Format-Table Id, DisplayName, Description -Wrap }
            exit 0
        }
        'create' {
            $created = New-HEProject -RepositoryRoot $repositoryRoot -ProjectPath $Project -Name $Name -Template $Template
            Add-HERecentProject $repositoryRoot $created
            $created | ConvertTo-Json; exit 0
        }
        'use' {
            if (-not $Project) { throw 'project use requires -Project.' }
            $selected = Set-HEWorkspaceProject -RepositoryRoot $repositoryRoot -ProjectPath $Project
            Add-HERecentProject $repositoryRoot $selected
            $selected | ConvertTo-Json; exit 0
        }
        'reset' {
            Write-HEWorkspace (Join-Path $repositoryRoot 'HEngine.workspace.local.json') ([ordered]@{SchemaVersion=1;DefaultProject=''})
            Write-Output 'Workspace default restored.'; exit 0
        }
        'list' { ConvertTo-Json -InputObject @(Get-HERecentProjects $repositoryRoot); exit 0 }
        'current' { }
        default { throw 'Use project manager|templates|create|use|current|list|reset.' }
    }
}
if ($Scope -ieq 'editor' -and $Action -ieq 'open' -and -not $Project -and -not $env:HE_PROJECT_DIR) {
    try { $null = Resolve-HEProject -RepositoryRoot $repositoryRoot }
    catch {
        Write-Warning $_.Exception.Message
        & (Join-Path $PSScriptRoot 'ProjectManager.ps1')
        exit 0
    }
}
if ($Scope.Trim() -ieq 'deps' -and $Profile -eq 'Source') {
    $Project = Join-Path $repositoryRoot 'Engine/Templates/Blank'
}
$projectInfo = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repositoryRoot
$bDefaultProject = Test-DefaultProject -ProjectRoot $projectInfo.Root -RepositoryRoot $repositoryRoot
if ($Scope -ieq 'project' -and $Action -ieq 'current') {
    $selectionSource = if ($Project) { '-Project' } elseif ($env:HE_PROJECT_DIR) { 'HE_PROJECT_DIR' } else { (Find-HEProjectSelection $repositoryRoot).Source }
    $selectedConfiguration = if ($PSBoundParameters.ContainsKey('Configuration')) { $Configuration.ToLowerInvariant() } else { 'relwithdebinfo' }
    [ordered]@{
        engine = $repositoryRoot; project = $projectInfo; selectedBy = $selectionSource
        configuration = $selectedConfiguration
        gameBuild = Get-ProjectBuildRoot game $selectedConfiguration
        editorBuild = Get-ProjectBuildRoot editor $selectedConfiguration
        agentSessions = if ($bDefaultProject) { Join-Path $repositoryRoot 'out/agent/sessions' } else { Join-Path (Get-ProjectBuildRoot game $selectedConfiguration) 'sessions' }
        saved = Join-Path $projectInfo.Root 'Saved'
    } | ConvertTo-Json -Depth 5
    exit 0
}
$buildScript = Join-Path $PSScriptRoot "Build.ps1"
$codegenScript = Join-Path $PSScriptRoot "CodeGen.ps1"
$testScript = Join-Path $PSScriptRoot "Run-Test.ps1"
$sessionScript = Join-Path $PSScriptRoot "Session.ps1"
$packageScript = Join-Path $repositoryRoot "Win-PackGame.bat"
$normalizedScope = $Scope.Trim().ToLowerInvariant()
$normalizedAction = $Action.Trim().ToLowerInvariant()

if ($normalizedScope -eq 'scripting') {
    if (-not $normalizedAction) { $normalizedAction = 'probe-pc' }
    & (Join-Path $repositoryRoot 'Tools/Scripting/Scripting.ps1') -Action $normalizedAction
    exit $LASTEXITCODE
}

if ($normalizedScope -eq 'wechat') {
    if (-not $normalizedAction) { $normalizedAction = 'doctor' }
    & (Join-Path $repositoryRoot 'Tools/WeChat/WeChat.ps1') -Action $normalizedAction -ProjectRoot $projectInfo.Root -Config $Source -Output $Report
    exit $LASTEXITCODE
}

# Runtime-facing commands should behave like the optimized manual launchers by
# default, while tests and diagnostic tooling retain their Debug defaults.
if (-not $PSBoundParameters.ContainsKey("Configuration") -and
    $normalizedScope -in @("game", "editor", "agent", "help", "-h", "--help")) {
    $Configuration = "RelWithDebInfo"
}

if ($normalizedScope -eq "asset") {
    if ($normalizedAction -notin @('plan', 'move', 'recover') -or
        ($normalizedAction -ne 'recover' -and (!$Source -or !$Destination))) {
        throw 'Use hectl asset plan|move -Source Assets/<old> -Destination Assets/<new>, or asset recover.'
    }
    $assetArgs = @((Join-Path $repositoryRoot 'Tools/Content/asset_operations.py'), $normalizedAction, '--project', $projectInfo.Root)
    if ($normalizedAction -ne 'recover') { $assetArgs += @('--source', $Source, '--target', $Destination) }
    & python @assetArgs
    exit $LASTEXITCODE
}

if ($normalizedScope -eq "lighting") {
    if ($normalizedAction -notin @("bake", "check")) {
        throw "Use hectl lighting bake|check [-Source Config/LightingBake.json] [-BlenderExe PATH]."
    }
    $lightingArgs = @((Join-Path $repositoryRoot 'Tools/Lighting/bake.py'), $normalizedAction,
        '--project', $projectInfo.Root, '--config', $(if ($Source) { $Source } else { 'Config/LightingBake.json' }))
    if ($BlenderExe) { $lightingArgs += @('--blender', $BlenderExe) }
    if ($PSBoundParameters.ContainsKey('TimeoutSeconds')) { $lightingArgs += @('--timeout', [string]$TimeoutSeconds) }
    if ($env:HE_LIGHTING_PYTHON) { & $env:HE_LIGHTING_PYTHON @lightingArgs }
    elseif (Get-Command py -ErrorAction SilentlyContinue) { & py -3 @lightingArgs }
    else { & python @lightingArgs }
    exit $LASTEXITCODE
}

if ($normalizedScope -eq "content") {
    if ($normalizedAction -notin @("sync", "plan", "diagnose") -or ($normalizedAction -ne "diagnose" -and -not $Source)) {
        Write-Output (@{ok=$false;status="parameter_error";errors=@("Use hectl content sync|plan -Source <file>, or diagnose -Report <report.json>.")} | ConvertTo-Json -Compress)
        exit 2
    }
    $contentRoot = Join-Path $repositoryRoot "Tools/Content"
    if (-not $Report -and $normalizedAction -ne "diagnose") {
        $Report = Join-Path $repositoryRoot ("out/agent/content/{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), [guid]::NewGuid().ToString('N').Substring(0,8))
    }
    if ($normalizedAction -ne "diagnose") {
        New-Item -ItemType Directory -Path $Report -Force | Out-Null
        if (-not $ImporterExe) {
            & $buildScript -Target HAssetImporter -Configuration RelWithDebInfo -Project $projectInfo.Root *> (Join-Path $Report "build.log")
            if ($LASTEXITCODE -ne 0) {
                Write-Output (@{ok=$false;status="build_failed";report=(Join-Path $Report "build.log")} | ConvertTo-Json -Compress)
                exit 3
            }
            $ImporterExe = Join-Path (Get-ProjectBuildRoot -Mode editor -ConfigurationName relwithdebinfo) "bin/Programs/HAssetImporter/HAssetImporter.exe"
        }
    }
    $contentArgs = @((Join-Path $contentRoot "content_sync.py"), $normalizedAction,
        "--project", $projectInfo.Root, "--verify", $Verify,
        "--timeout", "$TimeoutSeconds", "--report", $Report)
    if ($Source) { $contentArgs += @("--source", $Source) }
    if ($ImporterExe) { $contentArgs += @("--importer", $ImporterExe) }
    if ($BlenderExe) { $contentArgs += @("--blender", $BlenderExe) }
    if ($InitializeIds) { $contentArgs += "--initialize-ids" }
    if ($ObjectId) { $contentArgs += @("--object-id", $ObjectId) }
    if ($ErrorCode) { $contentArgs += @("--error-code", $ErrorCode) }
    & python @contentArgs
    exit $LASTEXITCODE
}

if ($WhatIf -and $normalizedScope -notin @("clean", "deps")) {
    throw "-WhatIf is only valid with: hectl clean stale -WhatIf"
}
if (($Json -or $Quiet) -and $normalizedScope -notin @("codegen", "clean", "deps")) {
    throw "--json and --quiet are valid with codegen and clean"
}

# The repository's default Editor dock layout is authored for a wide desktop.
# Keep the game/session default compact, but make Editor observations useful.
if ($normalizedScope -eq "editor" -and -not $PSBoundParameters.ContainsKey("Resolution")) {
    $Resolution = "1920x1080"
}

if ($normalizedScope -eq "help" -or $normalizedScope -eq "-h" -or
    $normalizedScope -eq "--help") {
    Show-Help
    exit 0
}

$previousProjectEnvironment = $env:HE_PROJECT_DIR
$env:HE_PROJECT_DIR = $projectInfo.Root
Push-Location $repositoryRoot
try {
    switch ($normalizedScope) {
        "publish" {
            if (!$normalizedAction) { $normalizedAction = 'check' }
            if ($normalizedAction -notin @('check','build')) { throw 'Use publish check|build -BuildProfile <id>.' }
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repositoryRoot 'Tools/Build/Publish.ps1') `
                -Project $projectInfo.Root -Profile $BuildProfile -Action $normalizedAction
            exit $LASTEXITCODE
        }
        "deps" {
            if (!$normalizedAction) { $normalizedAction = 'check' }
            & (Join-Path $PSScriptRoot '../Dependencies/Dependencies.ps1') -Action $normalizedAction `
                -Profile $Profile -Project $projectInfo.Root -PluginConfig $PluginConfig `
                -Offline:$Offline -WhatIf:$WhatIf -Json:$Json
            exit $LASTEXITCODE
        }
        "doctor" {
            if (-not [string]::IsNullOrWhiteSpace($normalizedAction)) {
                throw "Doctor takes no action argument. Use: hectl doctor"
            }
            Show-Doctor
        }
        "clean" {
            . (Join-Path $PSScriptRoot 'Storage.ps1')
            if (-not $normalizedAction) { $normalizedAction = 'status' }
            if ($normalizedAction -notin @('status','preview','stale','cache','caches','retired','maximize')) { throw 'Use clean status|preview|stale|cache|caches|retired|maximize.' }
            $storageArgs = @($normalizedAction)
            if ($normalizedAction -eq 'maximize' -and ($Category -or $Project -or $CachePath -or $KeepSuccess -ge 0 -or $FailureDays -ge 0 -or $BudgetBytes -ge 0)) {
                throw 'clean maximize targets this workspace out with its explicit retention rules; filters are not supported.'
            }
            if ($normalizedAction -eq 'cache') {
                if (-not $CachePath) { throw 'clean cache requires an explicit -CachePath; use -WhatIf first.' }
                $storageArgs += @('--cache-path',[IO.Path]::GetFullPath($CachePath))
            }
            if ($WhatIf) { $storageArgs += '--what-if' }
            if ($Category) { $storageArgs += @('--kind',$Category) }
            if ($KeepSuccess -ge 0) { $storageArgs += @('--success-count',[string]$KeepSuccess) }
            if ($FailureDays -ge 0) { $storageArgs += @('--failure-days',[string]$FailureDays) }
            if ($BudgetBytes -ge 0) { $storageArgs += @('--budget-bytes',[string]$BudgetBytes) }
            if ($Report) { $storageArgs += @('--report',$Report) }
            if ($Project) { $storageArgs += @('--project',$projectInfo.Root) }
            $storageOutput = Invoke-HEStorage $storageArgs
            if ($Json -or ($normalizedAction -eq 'status' -and -not $Quiet)) { Write-Output $storageOutput }
            elseif (-not $Quiet) {
                $storageResult = $storageOutput | ConvertFrom-Json
                if ($normalizedAction -eq 'maximize') {
                    Write-Host "Out cleanup: $($storageResult.outcome); reclaimable=$($storageResult.reclaimableBytes); deleted=$($storageResult.deletedBytes); skipped=$(@($storageResult.skipped).Count); failed=$(@($storageResult.failed).Count)"
                    Write-Host "Volume free bytes: before=$($storageResult.freeBytesBefore); after=$($storageResult.freeBytesAfter); change=$($storageResult.physicalFreeIncreaseBytes)"
                    $storageResult.skipped | Select-Object -First 12 path,reason | Format-Table -Wrap
                    if (-not $WhatIf) { Write-Host 'Report: out/agent/storage/last-maximize-clean.json' }
                    break
                }
                if ($normalizedAction -eq 'caches') {
                    $storageResult.caches | Select-Object path,@{Name='GiB';Expression={[math]::Round($_.bytes / 1GB, 2)}},lastWriteUtc,state,defaultHost | Format-Table -AutoSize
                    Write-Host "Logical cache total: $([math]::Round($storageResult.totalBytes / 1GB, 2)) GiB; run clean cache -CachePath <path> -WhatIf before deletion."
                    break
                }
                if ($normalizedAction -eq 'cache') { Write-Host "Cache unit: $($storageResult.paths -join ', ')" }
                if ($normalizedAction -eq 'retired') { $storageResult.retired | Select-Object disposition,bytes,path,reason | Format-Table -Wrap }
                $storageResult.items | Select-Object disposition,reason,bytes,paths | Format-Table -Wrap
                Write-Host "Logical bytes: reclaimable=$($storageResult.reclaimableBytes); deleted=$($storageResult.deletedBytes); failed=$(@($storageResult.failed).Count); protectedOverBudget=$($storageResult.protectedOverBudgetBytes)"
            }
        }
        "codegen" {
            if ([string]::IsNullOrWhiteSpace($normalizedAction)) {
                $normalizedAction = "status"
            }
            if ($normalizedAction -notin @("status", "check", "run")) {
                throw "Unknown codegen action '$Action'. Use status, check, or run."
            }
            $codegenOutputRoot = switch ($CodeGenHost) {
                "Editor" {
                    Join-Path (Get-ProjectBuildRoot -Mode "editor" `
                        -ConfigurationName $Configuration.ToLowerInvariant()) "GeneratedSource"
                }
                "DevGame" {
                    Join-Path (Get-ProjectBuildRoot -Mode "game" `
                        -ConfigurationName $Configuration.ToLowerInvariant()) "GeneratedSource"
                }
                "Tests" {
                    Join-Path (Get-ProjectBuildRoot -Mode "tests" `
                        -ConfigurationName $Configuration.ToLowerInvariant()) "GeneratedSource"
                }
                "Package" {
                    $packageBuildRoot = if ($bDefaultProject) {
                        Join-Path $repositoryRoot "out\package\build"
                    }
                    else {
                        Join-Path $repositoryRoot ("out\pkg\" + $projectInfo.PackageKey.Substring(0, 12))
                    }
                    Join-Path (Join-Path $packageBuildRoot "GeneratedSource") "Release"
                }
            }
            $codegenParameters = @{
                RepositoryRoot = $repositoryRoot
                ProjectRoot = $projectInfo.Root
                GeneratedSourceDir = $codegenOutputRoot
                Action = $normalizedAction
                CodeGenHost = $CodeGenHost
                Configuration = $Configuration
                Json = $Json
                Quiet = $Quiet
                UseManagedBuild = $true
            }
            & $codegenScript @codegenParameters
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
        "game" {
            if ([string]::IsNullOrWhiteSpace($normalizedAction)) {
                $normalizedAction = "build"
            }
            switch ($normalizedAction) {
                "build" {
                    Write-Host "route: HGame Dev (HE_BUILD_DEV_GAME=ON, PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HGame"
                }
                { $_ -eq "check" -or $_ -eq "run" } {
                    Write-Host "route: owned HGame Dev check (PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HGame" -Run -BoundedRun
                }
                "capture" {
                    Write-Host "route: owned deterministic HGame Dev capture (PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HGame" -Run -BoundedRun -Capture
                }
                "open" {
                    Write-Host "route: manual HGame Dev open (HE_ENABLE_GM=ON; process remains user-owned)"
                    Invoke-Build -Target "HGame" -Run -ShowBuildOutput
                }
                default {
                    throw "Unknown game action '$Action'. Use build, check, run, capture, or open."
                }
            }
        }
        "editor" {
            if ([string]::IsNullOrWhiteSpace($normalizedAction)) {
                $normalizedAction = "build"
            }
            switch ($normalizedAction) {
                "build" {
                    Write-Host "route: additional HEngineEditor build (PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HEngineEditor"
                }
                { $_ -eq "check" -or $_ -eq "run" } {
                    Write-Host "route: owned HEngineEditor check (PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HEngineEditor" -Run -BoundedRun
                }
                "capture" {
                    Write-Host "route: owned bounded HEngineEditor main-window capture (PACK_GAME=OFF, HE_ENABLE_GM=ON)"
                    Invoke-Build -Target "HEngineEditor" -Run -BoundedRun -Capture
                }
                "open" {
                    Write-Host "route: manual HEngineEditor open (HE_ENABLE_GM=ON; process remains user-owned)"
                    Invoke-Build -Target "HEngineEditor" -Run -ShowBuildOutput
                }
                default {
                    throw "Unknown editor action '$Action'. Use build, check, run, capture, or open."
                }
            }
        }
        "test" {
            Write-Host "route: headless HEngineTests (PACK_GAME=OFF, HE_BUILD_DEV_GAME=ON, HE_ENABLE_GM=ON)"
            if ([string]::IsNullOrWhiteSpace($normalizedAction) -or
                $normalizedAction -eq "list") {
                & $testScript -List -Project $projectInfo.Root -Configuration $Configuration -Parallel $Parallel `
                    -TimeoutSeconds $TimeoutSeconds -PluginConfig $PluginConfig
            }
            else {
                & $testScript -Name $Action -Project $projectInfo.Root -Configuration $Configuration -Parallel $Parallel `
                    -TimeoutSeconds $TimeoutSeconds -PluginConfig $PluginConfig
            }
            if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
        "agent" {
            Write-Host "route: Agent-owned HGame Dev session (PACK_GAME=OFF, HE_BUILD_DEV_GAME=ON, HE_ENABLE_GM=ON)"
            if ([string]::IsNullOrWhiteSpace($normalizedAction)) {
                $normalizedAction = "status"
            }
            $sessionParameters = @{
                DevelopmentTemplate = $DevelopmentTemplate
                BuildProfile = $BuildProfile
                UseWarp = $UseWarp
                ProjectRoot = $projectInfo.Root
                ArtifactPath = (Join-Path (Get-ProjectBuildRoot -Mode game -ConfigurationName $Configuration.ToLowerInvariant()) ("bin/{0}.exe" -f $projectInfo.ExecutableName))
                SessionRoot = $(if ($bDefaultProject) { Join-Path $repositoryRoot "out/agent/sessions" } else { Join-Path (Get-ProjectBuildRoot -Mode game -ConfigurationName $Configuration.ToLowerInvariant()) "sessions" })
                Configuration = $Configuration
                Parallel = $Parallel
                TimeoutSeconds = [Math]::Min($TimeoutSeconds, 300)
                FixedDeltaSeconds = $FixedDeltaSeconds
                Resolution = $Resolution
                UseProjectDisplaySettings = $UseProjectDisplaySettings
                RenderValidation = $RenderValidation
                RenderStats = $RenderStats
                RenderPath = $RenderPath
                RenderStatsPath = $RenderStatsPath
                RenderSettingsPath = $RenderSettingsPath
                ExistingBuildSha256 = $ExistingBuildSha256
                GraphicsApi = $GraphicsApi
                StrictGraphicsApi = $StrictGraphicsApi
                PerformanceWarmupFrames = $PerformanceWarmupFrames
                EnableTracy = $EnableTracy
            }
            if ($UseProjectDisplaySettings) { $sessionParameters.Remove('Resolution') }
            if (-not [string]::IsNullOrEmpty($AgentOwnerTag)) {
                $sessionParameters.OwnerTag = $AgentOwnerTag.ToLowerInvariant()
            }
            if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
                $sessionParameters.StartupScene = $StartupScene
            }
            switch ($normalizedAction) {
                "start" {
                    $sessionParameters.Action = "Start"
                }
                "status" {
                    $sessionParameters.Action = "Status"
                }
                "discover" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "api.discover"
                }
                "gm-discover" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "gm.discover"
                }
                "gm-execute" {
                    if ([string]::IsNullOrWhiteSpace($GMCommand)) {
                        throw "agent gm-execute requires -GMCommand."
                    }
                    try {
                        $gmArguments = $GMArgumentsJson | ConvertFrom-Json
                    }
                    catch {
                        throw "-GMArgumentsJson must be a JSON object: $($_.Exception.Message)"
                    }
                    if ($null -eq $gmArguments -or $gmArguments -is [System.Array] -or
                        $gmArguments -is [string] -or $gmArguments -is [ValueType]) {
                        throw "-GMArgumentsJson must be a JSON object."
                    }
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "gm.execute"
                    $sessionParameters.ParamsJson = [ordered]@{
                        command = $GMCommand
                        arguments = $gmArguments
                        confirm = $GMConfirm.IsPresent
                    } | ConvertTo-Json -Depth 32 -Compress
                }
                "world" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "world.summary"
                }
                "logs" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "logs.read"
                    $sessionParameters.ParamsJson = $ParamsJson
                }
                "pause" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "runtime.pause"
                }
                "resume" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "runtime.resume"
                }
                "step" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "runtime.step"
                    $sessionParameters.ParamsJson = "{`"frames`":$AgentStepFrames}"
                }
                "keys" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "input.keys"
                }
                "input" {
                    if ([string]::IsNullOrWhiteSpace($AgentHeldKeys)) {
                        throw "agent input requires -AgentHeldKeys with comma-separated key names."
                    }
                    $heldKeys = @($AgentHeldKeys.Split(',') | ForEach-Object { $_.Trim() })
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "input.set"
                    $sessionParameters.ParamsJson = (@{ held = $heldKeys } |
                        ConvertTo-Json -Compress)
                }
                "clear-input" {
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "input.clear"
                }
                "click" {
                    if ([double]::IsNaN($AgentClickX) -or [double]::IsInfinity($AgentClickX) -or
                        [double]::IsNaN($AgentClickY) -or [double]::IsInfinity($AgentClickY)) {
                        throw "agent click requires finite -AgentClickX and -AgentClickY values."
                    }
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "input.click"
                    $sessionParameters.ParamsJson = (@{
                        x = $AgentClickX
                        y = $AgentClickY
                        button = $AgentClickButton
                    } | ConvertTo-Json -Compress)
                }
                "capture" {
                    $captureName = $AgentCaptureName
                    if ([string]::IsNullOrWhiteSpace($captureName)) {
                        $captureName = "agent-frame-{0}.png" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff")
                    }
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = "capture.frame"
                    $sessionParameters.ParamsJson = @{ filename = $captureName } |
                        ConvertTo-Json -Compress
                }
                "call" {
                    if ([string]::IsNullOrWhiteSpace($Method)) {
                        throw "agent call requires -Method."
                    }
                    $sessionParameters.Action = "Call"
                    $sessionParameters.Method = $Method
                    $sessionParameters.ParamsJson = $ParamsJson
                }
                "stop" {
                    $sessionParameters.Action = "Stop"
                }
                default {
                    throw "Unknown agent action '$Action'. Use start, status, discover, gm-discover, gm-execute, world, logs, pause, resume, step, keys, input, clear-input, click, capture, call, or stop."
                }
            }
            & $sessionScript @sessionParameters
            if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
        "android" {
            if (!$normalizedAction) { $normalizedAction = 'build' }
            if ($normalizedAction -eq 'setup') {
                & (Join-Path $repositoryRoot 'Tools/Android/Setup.ps1') -AcceptLicenses:$AcceptAndroidLicenses
                return
            }
            if ($normalizedAction -notin @('check','build','package')) { throw 'Use hectl android setup|check|build|package.' }
            $androidAction = if ($normalizedAction -eq 'check') { 'Check' } else { 'Build' }
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repositoryRoot 'Tools/Build/Publish.ps1') -Project $projectInfo.Root -Profile android -Action $androidAction
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        "package" {
            if (-not [string]::IsNullOrWhiteSpace($normalizedAction)) {
                throw "Package takes no action argument. Use: hectl package"
            }
            Write-Host "route: Release cook/package (PACK_GAME=ON, HE_ENABLE_GM=OFF)"
            if ($GameConfig -or $PluginConfig) {
                $packageArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repositoryRoot 'Tools/Agent/Package.ps1'), '-Project', $projectInfo.Root)
                if ($GameConfig) { $packageArguments += @('-GameConfig', $GameConfig) }
                if ($PluginConfig) { $packageArguments += @('-PluginConfig', $PluginConfig) }
                & powershell.exe @packageArguments
            } else { & $packageScript $projectInfo.Root }
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
        default {
            throw "Unknown scope '$Scope'. Use doctor, clean, codegen, game, editor, agent, test, package, or help."
        }
    }
}
finally {
    Pop-Location
    if ($null -eq $previousProjectEnvironment) {
        Remove-Item Env:\HE_PROJECT_DIR -ErrorAction SilentlyContinue
    }
    else {
        $env:HE_PROJECT_DIR = $previousProjectEnvironment
    }
}

