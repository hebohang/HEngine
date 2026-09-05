param(
    [Parameter(Position = 0)]
    [string]$Scope = "game",

    [Parameter(Position = 1)]
    [string]$Action = "",

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 60,

    [ValidateRange(1, 10000000)]
    [long]$Frames = 120,

    [double]$FixedDeltaSeconds = 0.0166667,

    [string]$Resolution = "1280x720",

    [string]$CapturePath = "",

    [string]$Method = "",

    [string]$ParamsJson = "{}",

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

    [string]$Project = "",

    [ValidateSet("Editor", "DevGame", "Tests", "Package")]
    [string]$CodeGenHost = "Editor",

    [switch]$Json,

    [switch]$Quiet,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArguments
)

$ErrorActionPreference = "Stop"

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
        $descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $descriptorPath | ConvertFrom-Json
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
        $packageBytes = [System.Text.Encoding]::UTF8.GetBytes(
            ("${projectId}`n${normalizedRoot}").ToLowerInvariant())
        $packageKey = ([System.BitConverter]::ToString(
            $sha256.ComputeHash($packageBytes))).Replace('-', '').Substring(0, 16).ToLowerInvariant()
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
        PackageKey = $packageKey
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

  hectl                         Build optimized HGame Dev (RelWithDebInfo, PACK_GAME=OFF)
  hectl doctor                  Read compact toolchain/artifact/session state
  hectl clean status            Report Agent storage and the stale cleanup set
  hectl clean stale             Keep reusable Debug caches; remove stale output
  hectl clean stale -WhatIf     Preview the cleanup without deleting anything
  hectl game build              Build HGame Dev
  hectl game check              Agent-owned bounded game run
  hectl game check -GraphicsApi Vulkan -StrictGraphicsApi
                                Require a real Vulkan bounded run (no fallback)
  hectl game check -GraphicsApi DX12 -UseWarp
                                Require the DX12 WARP compatibility path
  hectl game run                Alias of game check
  hectl game capture            Bounded game run with a deterministic PNG
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
  hectl agent stop              Gracefully close the Agent-owned game
  hectl test list               List headless engine tests
  hectl test <name>             Run exactly one headless engine test
  hectl codegen status          Report manifest freshness without generating
  hectl codegen check           Exit nonzero when reflection outputs are dirty
  hectl codegen run             Generate transactionally when inputs are dirty
  hectl codegen status --json   Emit the stable machine-readable result schema
  hectl codegen run --quiet     Suppress successful terminal output
  hectl package                 Cook/package Release game (PACK_GAME=ON)

Project selection:
  -Project <directory|Project.heproject>
  default: $($script:projectInfo.Root)

Configuration defaults:
  game/editor/agent: RelWithDebInfo
  test/codegen/doctor: Debug
  override either with -Configuration Debug|RelWithDebInfo

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

function Get-AgentStoragePlan {
    $agentRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $script:repositoryRoot "out\agent"))
    $preservedNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @(
        "game-debug",
        "editor-debug",
        "game-relwithdebinfo",
        "editor-relwithdebinfo",
        "tests-debug",
        "tests-relwithdebinfo",
        "projects",
        "codegen",
        "deps",
        "downloads",
        "tools")) {
        $null = $preservedNames.Add($name)
    }

    $preserved = New-Object 'System.Collections.Generic.List[System.IO.FileSystemInfo]'
    $stale = New-Object 'System.Collections.Generic.List[System.IO.FileSystemInfo]'
    if (Test-Path -LiteralPath $agentRoot) {
        foreach ($item in Get-ChildItem -LiteralPath $agentRoot -Force) {
            if ($preservedNames.Contains($item.Name)) {
                $preserved.Add($item)
            }
            else {
                $stale.Add($item)
            }
        }
    }

    return [pscustomobject]@{
        Root = $agentRoot
        Preserved = $preserved.ToArray()
        Stale = $stale.ToArray()
    }
}

function Get-AgentStorageItemBytes {
    param([System.IO.FileSystemInfo]$Item)

    if (-not $Item.PSIsContainer) {
        return [long]$Item.Length
    }
    [long]$total = 0
    foreach ($file in Get-ChildItem -LiteralPath $Item.FullName -File -Recurse -Force) {
        $total += [long]$file.Length
    }
    return $total
}

function Show-AgentStoragePlan {
    param($Plan)

    [long]$preservedBytes = 0
    [long]$staleBytes = 0
    $preserved = foreach ($item in $Plan.Preserved) {
        $bytes = Get-AgentStorageItemBytes -Item $item
        $preservedBytes += $bytes
        [pscustomobject]@{ disposition = "keep"; name = $item.Name; bytes = $bytes }
    }
    $stale = foreach ($item in $Plan.Stale) {
        $bytes = Get-AgentStorageItemBytes -Item $item
        $staleBytes += $bytes
        [pscustomobject]@{ disposition = "remove"; name = $item.Name; bytes = $bytes }
    }
    $result = [ordered]@{
        root = $Plan.Root
        preservedBytes = $preservedBytes
        reclaimableBytes = $staleBytes
        items = @($preserved) + @($stale)
    }
    Write-Output ($result | ConvertTo-Json -Depth 4)
}

function Assert-AgentStorageIdle {
    $processNames = @(
        "cmake",
        "ninja",
        $script:projectInfo.ExecutableName,
        "HEngineEditor",
        "HEngineTests",
        "HEngineParser",
        "HShaderCompiler")
    $running = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $processNames -contains $_.ProcessName })
    if ($running.Count -gt 0) {
        $summary = ($running | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ", "
        throw "Agent storage cleanup requires idle build/runtime processes. Running: $summary"
    }
}

function Assert-AgentStorageCleanupTarget {
    param(
        [string]$AgentRoot,
        [System.IO.FileSystemInfo]$Item
    )

    $resolvedRoot = [System.IO.Path]::GetFullPath($AgentRoot).TrimEnd('\')
    $resolvedItem = [System.IO.Path]::GetFullPath($Item.FullName)
    $parent = [System.IO.Directory]::GetParent($resolvedItem)
    if ($null -eq $parent -or -not [string]::Equals(
        $parent.FullName.TrimEnd('\'),
        $resolvedRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cleanup target is not an immediate child of the Agent root: $resolvedItem"
    }
    if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Cleanup refuses a reparse-point target: $resolvedItem"
    }
    if ($Item.PSIsContainer) {
        $nestedReparsePoints = @(Get-ChildItem -LiteralPath $resolvedItem -Force -Recurse |
            Where-Object {
                ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
            })
        if ($nestedReparsePoints.Count -gt 0) {
            throw "Cleanup refuses a target containing reparse points: $resolvedItem"
        }
    }
}

function Assert-AgentStorageLocksAvailable {
    param($Plan)

    $lockNames = @(
        ".agent-build.lock",
        ".agent-execution.lock",
        ".agent-session-lifecycle.lock",
        ".tracy-profile.lock")
    foreach ($item in @($Plan.Preserved) + @($Plan.Stale)) {
        if (-not $item.PSIsContainer) {
            continue
        }
        foreach ($lockName in $lockNames) {
            foreach ($lockPath in Get-ChildItem -LiteralPath $item.FullName -Filter $lockName `
                -File -Force -Recurse -ErrorAction SilentlyContinue) {
                $stream = $null
                try {
                    $stream = [System.IO.File]::Open(
                        $lockPath.FullName,
                        [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::ReadWrite,
                        [System.IO.FileShare]::None)
                }
                catch {
                    throw "Agent storage lock is in use: $($lockPath.FullName)"
                }
                finally {
                    if ($null -ne $stream) {
                        $stream.Dispose()
                    }
                }
            }
        }
    }
}

function Remove-StaleAgentStorage {
    param(
        $Plan,
        [switch]$Preview
    )

    if ($Plan.Stale.Count -eq 0) {
        Write-Host "Agent storage is already clean."
        return
    }
    Assert-AgentStorageIdle
    Assert-AgentStorageLocksAvailable -Plan $Plan
    foreach ($item in $Plan.Stale) {
        Assert-AgentStorageCleanupTarget -AgentRoot $Plan.Root -Item $item
    }
    if ($Preview) {
        Write-Host "WhatIf: no Agent storage was removed."
        return
    }
    foreach ($item in $Plan.Stale) {
        Write-Host "remove: $($item.FullName)"
        Remove-Item -LiteralPath $item.FullName -Recurse -Force -ErrorAction Stop
    }
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
    $activeSessionPath = Join-Path $script:repositoryRoot "out\agent\sessions\active.json"
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
        GraphicsApi = $GraphicsApi
        StrictGraphicsApi = $StrictGraphicsApi
        UseWarp = $UseWarp
    }
    if ($Run) {
        $parameters.Run = $true
    }
    if ($ShowBuildOutput) {
        $parameters.ShowBuildOutput = $true
    }
    if ($BoundedRun) {
        $parameters.TimeoutSeconds = $TimeoutSeconds
        $parameters.ExitAfterFrames = $Frames
        $parameters.FixedDeltaSeconds = $FixedDeltaSeconds
        $parameters.NoVSync = $true
        $parameters.Resolution = $Resolution
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
$projectInfo = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repositoryRoot
$bDefaultProject = Test-DefaultProject -ProjectRoot $projectInfo.Root -RepositoryRoot $repositoryRoot
$buildScript = Join-Path $PSScriptRoot "Build.ps1"
$codegenScript = Join-Path $PSScriptRoot "CodeGen.ps1"
$testScript = Join-Path $PSScriptRoot "Run-Test.ps1"
$sessionScript = Join-Path $PSScriptRoot "Session.ps1"
$packageScript = Join-Path $repositoryRoot "Win-PackGame.bat"
$normalizedScope = $Scope.Trim().ToLowerInvariant()
$normalizedAction = $Action.Trim().ToLowerInvariant()

# Runtime-facing commands should behave like the optimized manual launchers by
# default, while tests and diagnostic tooling retain their Debug defaults.
if (-not $PSBoundParameters.ContainsKey("Configuration") -and
    $normalizedScope -in @("game", "editor", "agent", "help", "-h", "--help")) {
    $Configuration = "RelWithDebInfo"
}

if (-not $bDefaultProject -and $normalizedScope -in @("agent", "test")) {
    throw "The legacy $normalizedScope helper still owns fixed default-project paths. Use game/editor/package with -Project, or invoke Build.ps1 directly for project-aware engine test builds."
}

if ($WhatIf -and $normalizedScope -ne "clean") {
    throw "-WhatIf is only valid with: hectl clean stale -WhatIf"
}
if (($Json -or $Quiet) -and $normalizedScope -ne "codegen") {
    throw "--json and --quiet are only valid with: hectl codegen status|check|run"
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
        "doctor" {
            if (-not [string]::IsNullOrWhiteSpace($normalizedAction)) {
                throw "Doctor takes no action argument. Use: hectl doctor"
            }
            Show-Doctor
        }
        "clean" {
            if ([string]::IsNullOrWhiteSpace($normalizedAction)) {
                $normalizedAction = "status"
            }
            $storagePlan = Get-AgentStoragePlan
            Show-AgentStoragePlan -Plan $storagePlan
            switch ($normalizedAction) {
                "status" {
                    if ($WhatIf) {
                        throw "-WhatIf is only valid with: hectl clean stale -WhatIf"
                    }
                }
                "stale" {
                    Remove-StaleAgentStorage -Plan $storagePlan -Preview:$WhatIf
                }
                default {
                    throw "Unknown clean action '$Action'. Use status or stale."
                }
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
                & $testScript -List -Configuration $Configuration -Parallel $Parallel `
                    -TimeoutSeconds $TimeoutSeconds
            }
            else {
                & $testScript -Name $Action -Configuration $Configuration -Parallel $Parallel `
                    -TimeoutSeconds $TimeoutSeconds
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
                Configuration = $Configuration
                Parallel = $Parallel
                TimeoutSeconds = [Math]::Min($TimeoutSeconds, 300)
                FixedDeltaSeconds = $FixedDeltaSeconds
                Resolution = $Resolution
                RenderValidation = $RenderValidation
                RenderStats = $RenderStats
                EnableTracy = $EnableTracy
            }
            if (-not [string]::IsNullOrEmpty($AgentOwnerTag)) {
                $sessionParameters.OwnerTag = $AgentOwnerTag.ToLowerInvariant()
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
        "package" {
            if (-not [string]::IsNullOrWhiteSpace($normalizedAction)) {
                throw "Package takes no action argument. Use: hectl package"
            }
            Write-Host "route: Release cook/package (PACK_GAME=ON, HE_ENABLE_GM=OFF)"
            & $packageScript $projectInfo.Root
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
