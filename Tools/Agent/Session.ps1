param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Start", "Call", "Status", "Stop")]
    [string]$Action,

    [string]$Method = "session.status",

    [string]$ParamsJson = "{}",

    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [ValidateRange(1, 300)]
    [int]$TimeoutSeconds = 30,

    [double]$FixedDeltaSeconds = 0.0166667,

    # Performance gameplay must use elapsed wall time, not accelerate simulation
    # when the renderer runs faster than the fixed-delta reference rate.
    [switch]$Realtime,

    [string]$Resolution = "1280x720",

    # Exercise the project's normal startup display mode and Saved preferences.
    # Omit resolution/mode overrides so UI fullscreen changes remain effective.
    [switch]$UseProjectDisplaySettings,

    # Visual/presentation QA may need the user's real synchronization policy.
    # Performance sessions retain their existing unthrottled default.
    [switch]$RespectPresentationSettings,

    [string]$StartupScene = "",

    [switch]$CaptureCursor,

    [string]$ProjectRoot = "",
    [string]$ArtifactPath = "",
    [string]$SessionRoot = "",
    [string]$DevelopmentTemplate = "",
    [string]$BuildProfile = "windows",

    [ValidateSet("", "DX12", "Vulkan")]
    [string]$GraphicsApi = "",

    [switch]$StrictGraphicsApi,
    [switch]$UseWarp,

    [switch]$RenderValidation,

    [switch]$RenderStats,

    [ValidateSet("", "Forward", "Deferred")]
    [string]$RenderPath = "",

    [string]$RenderStatsPath = "",

    [string]$RenderSettingsPath = "",

    # Seed only JSON preferences into a fresh owned Saved directory before launch.
    # Used to verify a new process reads preferences saved by a previous owned run.
    [string]$SavedConfigSource = "",

    # Explicitly pin a previously validated isolated binary for repeated runtime
    # checks while source work continues. This is never current-source build proof.
    [ValidatePattern('^$|^[0-9a-fA-F]{64}$')]
    [string]$ExistingBuildSha256 = "",

    [ValidateRange(0, 10000000)]
    [long]$PerformanceWarmupFrames = 30,

    [switch]$EnableTracy,

    [ValidateScript({
        if ([string]::IsNullOrEmpty($_) -or $_ -match '^[0-9a-fA-F]{32}$') {
            return $true
        }
        throw "-OwnerTag must be empty or exactly 32 hexadecimal characters."
    })]
    [string]$OwnerTag = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Process.ps1")

function Get-SessionIoPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute.StartsWith('\\?\')) { return $absolute }
    if ($absolute.StartsWith('\\')) { return '\\?\UNC\' + $absolute.Substring(2) }
    return '\\?\' + $absolute
}

function Write-JsonAtomically {
    param(
        [string]$Path,
        [string]$Contents
    )

    $parent = Split-Path -Parent $Path
    $null = [IO.Directory]::CreateDirectory((Get-SessionIoPath $parent))
    $temporary = "$Path.tmp.$PID.$([guid]::NewGuid().ToString('N'))"
    try {
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $temporaryIo = Get-SessionIoPath $temporary
        $destinationIo = Get-SessionIoPath $Path
        [System.IO.File]::WriteAllText($temporaryIo, $Contents, $encoding)
        if ([IO.File]::Exists($destinationIo)) { [IO.File]::Replace($temporaryIo,$destinationIo,[NullString]::Value) }
        else { [IO.File]::Move($temporaryIo,$destinationIo) }
    }
    finally {
        if ([IO.File]::Exists((Get-SessionIoPath $temporary))) {
            [IO.File]::Delete((Get-SessionIoPath $temporary))
        }
    }
}

function New-SessionToken {
    $bytes = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($bytes)
    }
    finally {
        $generator.Dispose()
    }
    return (-join ($bytes | ForEach-Object { $_.ToString("x2") }))
}

function Read-ActiveSession {
    if (-not (Test-Path -LiteralPath $script:activeSessionPath)) {
        throw "No active Agent session is recorded. Start one with: hectl agent start"
    }
    return (Get-Content -Raw -Encoding UTF8 -LiteralPath $script:activeSessionPath | ConvertFrom-Json)
}

function Assert-SessionOwnerTag {
    param(
        [Parameter(Mandatory = $true)]
        $Owner
    )

    if ([string]::IsNullOrEmpty($OwnerTag)) {
        return
    }

    $expectedOwnerTag = $OwnerTag.ToLowerInvariant()
    if ($null -eq $Owner.PSObject.Properties["ownerTag"] -or
        -not [string]::Equals(
            [string]$Owner.ownerTag,
            $expectedOwnerTag,
            [System.StringComparison]::Ordinal)) {
        throw "The active Agent session ownerTag does not match the requested owner."
    }
}

function Get-RecordedProcess {
    param($Owner)

    $process = Get-Process -Id ([int]$Owner.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $null
    }
    try {
        $actualStartTicks = $process.StartTime.ToUniversalTime().Ticks
    }
    catch {
        return $null
    }
    if ($actualStartTicks -ne [long]$Owner.processStartUtcTicks) {
        return $null
    }
    return $process
}

function Get-RecordedJobName {
    param($Owner)

    if ($null -eq $Owner.PSObject.Properties["jobName"]) {
        return $null
    }
    $jobName = [string]$Owner.jobName
    if ($jobName -notmatch '^Local\\HEngineAgentSession_[0-9a-f]{32}$') {
        throw "The recorded Agent session Job name is invalid."
    }
    return $jobName
}

function Stop-RecordedSessionJob {
    [OutputType([bool])]
    param($Owner)

    $jobName = Get-RecordedJobName -Owner $Owner
    if ([string]::IsNullOrWhiteSpace($jobName)) {
        return $false
    }
    return (Stop-AgentProcessJobByName -Name $jobName)
}

function Set-RecordedSessionState {
    param(
        $Owner,
        [string]$State
    )

    if (-not (Test-Path -LiteralPath $script:activeSessionPath)) {
        return
    }
    $currentOwner = Get-Content -Raw -Encoding UTF8 -LiteralPath $script:activeSessionPath |
        ConvertFrom-Json
    $bSameOwner = [int]$currentOwner.pid -eq [int]$Owner.pid -and
        [long]$currentOwner.processStartUtcTicks -eq [long]$Owner.processStartUtcTicks -and
        [string]::Equals(
            [string]$currentOwner.sessionDirectory,
            [string]$Owner.sessionDirectory,
            [System.StringComparison]::OrdinalIgnoreCase)
    if (-not $bSameOwner) {
        return
    }

    $Owner.state = $State
    Write-JsonAtomically -Path $script:activeSessionPath `
        -Contents ($Owner | ConvertTo-Json -Depth 8 -Compress)
}

function Open-SessionLifecycleLock {
    try {
        return [System.IO.File]::Open(
            $script:sessionLifecycleLockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another Agent session lifecycle operation is already in progress."
    }
}

function Read-JsonFileWithRetry {
    param(
        [string]$Path,
        [int]$TimeoutMilliseconds = 1000
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        if ([IO.File]::Exists((Get-SessionIoPath $Path))) {
            try {
                $json = [IO.File]::ReadAllText((Get-SessionIoPath $Path),[Text.Encoding]::UTF8)
                $value = $json | ConvertFrom-Json
                return @{ Json = $json; Value = $value }
            }
            catch {
            }
        }
        Start-Sleep -Milliseconds 25
    } while ($stopwatch.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    throw "Timed out reading JSON state: $Path"
}

function Invoke-AgentCall {
    param(
        $Owner,
        [string]$CallMethod,
        [string]$CallParamsJson,
        [switch]$Quiet,
        [DateTime]$DeadlineUtc = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    )

    if ($CallMethod -notmatch "^[A-Za-z0-9_.-]{1,96}$") {
        throw "Agent method has an invalid format: $CallMethod"
    }
    try {
        $params = $CallParamsJson | ConvertFrom-Json
    }
    catch {
        throw "-ParamsJson must be a JSON object: $($_.Exception.Message)"
    }
    if ($null -eq $params -or $params -is [System.Array] -or
        $params -is [string] -or $params -is [ValueType]) {
        throw "-ParamsJson must be a JSON object."
    }

    $process = Get-RecordedProcess -Owner $Owner
    if ($null -eq $process) {
        throw "The recorded Agent session process is no longer running."
    }

    $handle = "r{0}_{1}" -f (Get-Date -Format "yyyyMMddHHmmssfff"), [guid]::NewGuid().ToString("N")
    $request = [ordered]@{
        protocol = 1
        id = $handle
        method = $CallMethod
        token = [string]$Owner.token
        params = $params
    }
    $requestJson = $request | ConvertTo-Json -Depth 32 -Compress
    if ([System.Text.Encoding]::UTF8.GetByteCount($requestJson) -gt 65536) {
        throw "Agent request exceeds the 64 KiB protocol limit."
    }

    $sessionDirectory = [string]$Owner.sessionDirectory
    $requestPath = Join-Path $sessionDirectory "requests\$handle.request.json"
    $responsePath = Join-Path $sessionDirectory "responses\$handle.response.json"
    Write-JsonAtomically -Path $requestPath -Contents $requestJson

    while ([DateTime]::UtcNow -lt $DeadlineUtc) {
        if ([IO.File]::Exists((Get-SessionIoPath $responsePath))) {
            $responseJson = [IO.File]::ReadAllText((Get-SessionIoPath $responsePath),[Text.Encoding]::UTF8)
            $response = $responseJson | ConvertFrom-Json
            if ([string]$response.id -ne $handle) {
                throw "Agent response id did not match its request."
            }
            return $response
        }

        $process.Refresh()
        if ($process.HasExited) {
            Get-Content -LiteralPath ([string]$Owner.stdoutLog) -Tail 50 -ErrorAction SilentlyContinue
            Get-Content -LiteralPath ([string]$Owner.stderrLog) -Tail 50 -ErrorAction SilentlyContinue
            throw "Agent game process exited before responding (exit=$($process.ExitCode))."
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Agent call timed out after $TimeoutSeconds seconds: $CallMethod"
}

. (Join-Path $PSScriptRoot 'Storage.ps1')

function Start-AgentSession {
    if (Test-Path -LiteralPath $script:activeSessionPath) {
        $previousOwner = Read-ActiveSession
        if ($null -ne (Get-RecordedProcess -Owner $previousOwner)) {
            throw "An Agent session is already running (pid=$($previousOwner.pid)). Stop it before starting another."
        }
        [void](Stop-RecordedSessionJob -Owner $previousOwner)
    }

    $sdkPlayer = $DevelopmentTemplate -or (Test-Path -LiteralPath "$script:repositoryRoot/HEngine.distribution.json")
    if ($sdkPlayer -and ($EnableTracy -or $ExistingBuildSha256)) { throw 'SDK sessions use the verified development template; Tracy and ExistingBuildSha256 require a source build.' }
    $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), [guid]::NewGuid().ToString("N").Substring(0, 12)
    $runDirectory = Join-Path $script:sessionsRoot $runId
    $sessionDirectory = Join-Path $runDirectory "mailbox"
    New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
    $stdoutLog = Join-Path $runDirectory "game.stdout.log"
    $stderrLog = Join-Path $runDirectory "game.stderr.log"
    $savedRoot = Join-Path $runDirectory "Saved"
    New-Item -ItemType Directory -Force -Path $savedRoot | Out-Null
    $prepared = $null
    if ($sdkPlayer) {
        $prepared = & "$script:repositoryRoot/Tools/Distribution/Prepare-DevPlayer.ps1" -Project $ProjectRoot `
            -RunDirectory $runDirectory -BuildProfile $BuildProfile -Template $DevelopmentTemplate
    } elseif (-not $ExistingBuildSha256) {
        & $script:buildScript -Target HGame -Configuration $Configuration -Parallel $Parallel `
            -EnableTracy:$EnableTracy -Project $ProjectRoot
        if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    $modeName = if ($EnableTracy) { "game-tracy" } else { "game" }
    $artifact = Join-Path $script:repositoryRoot "out\agent\$modeName-$($Configuration.ToLowerInvariant())\bin\StarbloomSurvivor.exe"
    if ($ArtifactPath) { $artifact = $ArtifactPath }
    if ($prepared) { $artifact = $prepared.Artifact }
    if (-not (Test-Path -LiteralPath $artifact)) {
        throw "HGame artifact was not found: $artifact"
    }
    # VS environment import can replace PSModulePath, disabling Get-FileHash
    # autoload in Windows PowerShell. Hash the exact binary without module lookup.
    $artifactStream = [IO.File]::OpenRead($artifact)
    $artifactHasher = [Security.Cryptography.SHA256]::Create()
    try { $artifactSha256 = [BitConverter]::ToString($artifactHasher.ComputeHash($artifactStream)).Replace('-', '') }
    finally { $artifactHasher.Dispose(); $artifactStream.Dispose() }
    if ($ExistingBuildSha256 -and $artifactSha256 -ne $ExistingBuildSha256) {
        throw "Pinned HGame binary hash differs; refusing to run another build."
    }
    if ($ExistingBuildSha256) {
        Write-Host "runtime-only: pinned isolated HGame SHA256=$artifactSha256; current source was not built"
    }

    if (-not [string]::IsNullOrWhiteSpace($SavedConfigSource)) {
        $configSource = (Resolve-Path -LiteralPath $SavedConfigSource -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $configSource -PathType Container)) {
            throw '-SavedConfigSource must name a directory containing saved JSON preferences.'
        }
        $configDestination = Join-Path $savedRoot 'Config'
        New-Item -ItemType Directory -Force -Path $configDestination | Out-Null
        foreach ($configFile in Get-ChildItem -LiteralPath $configSource -Filter '*.json' -File) {
            Copy-Item -LiteralPath $configFile.FullName -Destination (Join-Path $configDestination $configFile.Name)
        }
    }
    $token = New-SessionToken
    $jobName = "Local\HEngineAgentSession_$([guid]::NewGuid().ToString('N'))"
    $fixedDeltaText = $FixedDeltaSeconds.ToString("R", [System.Globalization.CultureInfo]::InvariantCulture)
    $arguments = @(
        "--agent-session=`"$sessionDirectory`"",
        "--agent-token=$token",
        "--agent-start-paused",
        "--saved-root=`"$savedRoot`""
    )
    if ($prepared) { $arguments += @("--project=`"$($prepared.ProjectRoot)`"", "--game-config=`"$($prepared.GameConfig)`"") }
    if (-not $Realtime) { $arguments += "--fixed-delta=$fixedDeltaText" }
    if (-not $RespectPresentationSettings) { $arguments += '--no-vsync' }
    if (-not $UseProjectDisplaySettings) { $arguments += "--resolution=$Resolution" }
    if (-not $CaptureCursor) { $arguments += "--no-cursor-capture" }
    if ($RenderValidation) {
        $arguments += "--render-validation"
    }
    if ($RenderStats) {
        $arguments += "--render-stats"
    }
    if (-not [string]::IsNullOrWhiteSpace($GraphicsApi)) {
        $arguments += "--graphics-api=$($GraphicsApi.ToLowerInvariant())"
    }
    if ($StrictGraphicsApi) { $arguments += "--strict-graphics-api" }
    if ($UseWarp) { $arguments += '--warp' }
    if (-not [string]::IsNullOrWhiteSpace($RenderPath)) {
        $arguments += "--render-path=$($RenderPath.ToLowerInvariant())"
    }
    if (-not [string]::IsNullOrWhiteSpace($RenderSettingsPath)) {
        $RenderSettingsPath = [System.IO.Path]::GetFullPath($RenderSettingsPath)
        if (-not (Test-Path -LiteralPath $RenderSettingsPath -PathType Leaf)) {
            throw "Render settings JSON was not found: $RenderSettingsPath"
        }
        $arguments += "--render-settings=`"$RenderSettingsPath`""
    }
    if (-not [string]::IsNullOrWhiteSpace($RenderStatsPath)) {
        $RenderStatsPath = [System.IO.Path]::GetFullPath($RenderStatsPath)
        $arguments += "--render-stats-path=`"$RenderStatsPath`""
    }
    $arguments += "--performance-warmup-frames=$PerformanceWarmupFrames"
    if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
        $arguments += "--startup-scene=$StartupScene"
    }

    $executionLockPath = Join-Path (Split-Path -Parent (Split-Path -Parent $artifact)) ".agent-execution.lock"
    $sessionStorage = Start-HEStorageRun -Project $ProjectRoot -Workflow 'agent-session' -Paths @($runDirectory) -Locks @($executionLockPath, $script:sessionLifecycleLockPath)
    $executionLock = $null
    $cacheLease = $null
    try {
        $cacheLease = Open-HECacheLease -CachePath (Split-Path -Parent $executionLockPath)
        try {
            $executionLock = [System.IO.File]::Open(
                $executionLockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        }
        catch {
            throw "Another Agent build or owned run is using the HGame directory. Wait for it to finish."
        }

        $runningArtifact = Get-AgentArtifactProcess `
            -ArtifactPath $artifact -ProcessName ([System.IO.Path]::GetFileNameWithoutExtension($artifact))
        if ($null -ne $runningArtifact) {
            throw "The stable HGame executable started running before the Agent session acquired ownership: pid=$($runningArtifact.Id)."
        }

        $process = $null
        $processJob = $null
        $owner = $null
        $bLeaveRunning = $false
        try {
            $processJob = New-AgentProcessJob -Name $jobName
            $process = Start-Process -FilePath $artifact -ArgumentList $arguments `
                -WorkingDirectory $script:repositoryRoot -RedirectStandardOutput $stdoutLog `
                -RedirectStandardError $stderrLog -PassThru
            $null = $process.Handle
            Add-AgentProcessToJob -Job $processJob -Process $process
            Set-HEStorageOwner -Run $sessionStorage -Process $process
            $owner = [ordered]@{
                storageId = $(if ($sessionStorage) { $sessionStorage.Id } else { '' })
                protocolVersion = 1
                state = "starting"
                runDirectory = $runDirectory
                sessionDirectory = $sessionDirectory
                token = $token
                pid = $process.Id
                processStartUtcTicks = $process.StartTime.ToUniversalTime().Ticks
                jobName = $jobName
                artifact = $artifact
                artifactSha256 = $artifactSha256
                pinnedExistingBuild = [bool]$ExistingBuildSha256
                useProjectDisplaySettings = [bool]$UseProjectDisplaySettings
                realtime = [bool]$Realtime
                tracyEnabled = [bool]$EnableTracy
                renderValidation = [bool]$RenderValidation
                renderStats = [bool]$RenderStats
                stdoutLog = $stdoutLog
                stderrLog = $stderrLog
            }
            if (-not [string]::IsNullOrEmpty($OwnerTag)) {
                $owner["ownerTag"] = $OwnerTag.ToLowerInvariant()
            }
            Write-JsonAtomically -Path (Join-Path $runDirectory 'owner.json') -Contents ($owner | ConvertTo-Json -Depth 8 -Compress)
            Write-JsonAtomically -Path $script:activeSessionPath `
                -Contents ($owner | ConvertTo-Json -Depth 8 -Compress)

            $statusPath = Join-Path $sessionDirectory "status.json"
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            $bReady = $false
            while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
                if (Test-Path -LiteralPath $statusPath) {
                    try {
                        $status = Get-Content -Raw -Encoding UTF8 -LiteralPath $statusPath | ConvertFrom-Json
                        $bReady = [string]$status.state -eq "ready"
                    }
                    catch {
                        $bReady = $false
                    }
                    if ($bReady) {
                        break
                    }
                }
                $process.Refresh()
                if ($process.HasExited) {
                    break
                }
                Start-Sleep -Milliseconds 50
            }
            if (-not $bReady) {
                Get-Content -LiteralPath $stdoutLog -Tail 50 -ErrorAction SilentlyContinue
                Get-Content -LiteralPath $stderrLog -Tail 50 -ErrorAction SilentlyContinue
                throw "HGame did not publish a ready Agent session within $TimeoutSeconds seconds."
            }

            $owner.state = "ready"
            Write-JsonAtomically -Path $script:activeSessionPath `
                -Contents ($owner | ConvertTo-Json -Depth 8 -Compress)
            $bLeaveRunning = $true
            Write-Host "agent session: READY pid=$($process.Id)"
            Write-Host "session: $sessionDirectory"
        }
        finally {
            try {
                if ($null -ne $process -and -not $bLeaveRunning) {
                    $jobStopError = $null
                    if ($null -ne $processJob) {
                        try {
                            [void](Stop-AgentProcessJobByName -Name $jobName)
                        }
                        catch {
                            $jobStopError = $_
                        }
                    }

                    $process.Refresh()
                    if (-not $process.HasExited -and -not $process.WaitForExit(5000)) {
                        Stop-AgentOwnedProcess -Process $process -Description "Agent HGame session"
                    }
                    if ($null -ne $jobStopError) {
                        throw $jobStopError
                    }
                }
            }
            finally {
                try {
                    if ($null -ne $processJob) {
                        Close-AgentProcessJob -Job $processJob
                    }
                }
                finally {
                    if ($null -ne $owner -and -not $bLeaveRunning) {
                        Set-RecordedSessionState -Owner $owner -State "stopped"
                    }
                }
            }
        }
    }
    finally {
        if ($null -ne $executionLock) {
            $executionLock.Dispose()
        }
        if ($cacheLease) { $cacheLease.Dispose() }
        if ($sessionStorage -and -not $bLeaveRunning) {
            $failedStartState = 'failed'
            if ($process) { try { $process.Refresh(); if (-not $process.HasExited) { $failedStartState = 'unknown' } } catch { $failedStartState = 'unknown' } }
            Complete-HEStorageRun -Run $sessionStorage -State $failedStartState -NoAuto
        }
    }
}

function Stop-AgentSession {
    param(
        [Parameter(Mandatory = $true)]
        $Owner
    )

    $owner = $Owner
    $process = Get-RecordedProcess -Owner $owner
    if ($null -eq $process) {
        [void](Stop-RecordedSessionJob -Owner $owner)
        Set-RecordedSessionState -Owner $owner -State "stopped"
        if ($owner.storageId) { Complete-HEStorageRun -Run ([pscustomobject]@{Id=$owner.storageId;Outer=$false}) -State unknown -NoAuto }
        if ($owner.renderValidation) {
            . (Join-Path $PSScriptRoot "Rendering-Validation.ps1")
            Assert-RenderingValidationLogs -Paths @([string]$owner.stdoutLog, [string]$owner.stderrLog)
        }
        Write-Host "agent session: already stopped"
        return
    }

    # Retain the native handle before a short-lived graceful exit so Windows
    # PowerShell can still read its actual exit code after WaitForExit.
    $null = $process.Handle

    try {
        $response = Invoke-AgentCall -Owner $owner -CallMethod "session.stop" `
            -CallParamsJson "{}" -Quiet
        if (-not [bool]$response.ok) {
            Write-Warning "session.stop was rejected; closing the owned process directly."
        }
    }
    catch {
        Write-Warning "Graceful Agent stop failed; closing the owned process directly: $($_.Exception.Message)"
    }

    # Statistics are deliberately buffered during measurement and serialized at
    # shutdown. A long capture can take more than five seconds to flush its CPU
    # graph rows; killing it here truncates evidence after an otherwise clean run.
    $gracefulExitMilliseconds = if ($owner.renderStats) { 60000 } else { 5000 }
    if (-not $process.WaitForExit($gracefulExitMilliseconds)) {
        Write-Warning "Agent graceful exit exceeded $gracefulExitMilliseconds ms; terminating the owned session."
        if (Stop-RecordedSessionJob -Owner $owner) {
            if (-not $process.WaitForExit(5000)) {
                throw "Agent HGame session did not exit after terminating its process Job."
            }
            $process.WaitForExit()
        }
        else {
            Stop-AgentOwnedProcess -Process $process -Description "Agent HGame session"
        }
    }
    else {
        $process.WaitForExit()
    }
    Set-RecordedSessionState -Owner $owner -State "stopped"
    Write-Host "agent session: STOPPED pid=$($owner.pid)"
    $sessionStorageState = 'failed'
    try {
        if ($owner.renderValidation) {
            . (Join-Path $PSScriptRoot "Rendering-Validation.ps1")
            Assert-RenderingValidationLogs -Paths @([string]$owner.stdoutLog, [string]$owner.stderrLog)
        }
        $process.Refresh()
        if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
            throw "Agent HGame exited unsuccessfully: exit=$($process.ExitCode). Logs: $($owner.stdoutLog), $($owner.stderrLog)"
        }
        $sessionStorageState = 'success'
    }
    finally {
        if ($owner.storageId) { Complete-HEStorageRun -Run ([pscustomobject]@{Id=$owner.storageId;Outer=$false}) -State $sessionStorageState -NoAuto }
    }
}

if ([double]::IsNaN($FixedDeltaSeconds) -or
    [double]::IsInfinity($FixedDeltaSeconds) -or
    $FixedDeltaSeconds -le 0.0 -or $FixedDeltaSeconds -gt 1.0) {
    throw "-FixedDeltaSeconds must be in the range (0, 1]."
}
if ($UseProjectDisplaySettings -and $PSBoundParameters.ContainsKey('Resolution')) {
    throw "-UseProjectDisplaySettings cannot be combined with an explicit -Resolution."
}
if (-not $UseProjectDisplaySettings -and $Resolution -notmatch "^[1-9][0-9]{0,3}[xX][1-9][0-9]{0,3}$") {
    throw "-Resolution must use WIDTHxHEIGHT."
}
$resolutionParts = $Resolution -split "[xX]"
if (-not $UseProjectDisplaySettings -and ([int]$resolutionParts[0] -gt 8192 -or [int]$resolutionParts[1] -gt 8192)) {
    throw "-Resolution dimensions must be in the range 1..8192."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildScript = Join-Path $PSScriptRoot "Build.ps1"
$sessionsRoot = Join-Path $repositoryRoot "out\agent\sessions"
if ($SessionRoot) { $sessionsRoot = $SessionRoot }
$activeSessionPath = Join-Path $sessionsRoot "active.json"
$sessionLifecycleLockPath = Join-Path $sessionsRoot ".agent-session-lifecycle.lock"
New-Item -ItemType Directory -Force -Path $sessionsRoot | Out-Null

function Resolve-SequenceHistoryArtifact($capture, $sequenceOwner) {
    $descriptor=$capture.sequenceHistoryArtifact
    if($null -eq $descriptor){return $null}
    if($descriptor.schema -ne 'he.atomic-sequence-history.v1' -or [long]$descriptor.recordCount -lt 0 -or [long]$descriptor.recordCount -gt 616 -or [long]$descriptor.bytes -lt 1 -or [long]$descriptor.bytes -gt 4MB){throw 'Invalid sequence history descriptor.'}
    $expected='captures/'+[IO.Path]::GetFileName([string]$capture.frames[0].relativePath)+'.sequence-history.json'
    if([string]$descriptor.relativePath -cne $expected){throw 'Unexpected sequence history path.'}
    $captureRoot=[IO.Path]::GetFullPath((Join-Path ([string]$sequenceOwner.sessionDirectory) 'captures'))
    $path=[IO.Path]::GetFullPath((Join-Path ([string]$sequenceOwner.sessionDirectory) $expected))
    if(!$path.StartsWith($captureRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Sequence history escaped capture root.'}
    $directory=Get-Item -LiteralPath $captureRoot -ErrorAction Stop
    $file=Get-Item -LiteralPath $path -ErrorAction Stop
    if(($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $file.PSIsContainer -or $file.Length -ne [long]$descriptor.bytes){throw 'Invalid sequence history artifact file.'}
    $data=[IO.File]::ReadAllText($path,[Text.Encoding]::UTF8)|ConvertFrom-Json
    if($data.schema -ne $descriptor.schema -or $data.ticket -ne $capture.ticket -or @($data.records).Count -ne [long]$descriptor.recordCount){throw 'Sequence history content/receipt mismatch.'}
    # Keep bulk history out of the bounded protocol response and CLI JSON as well.
    $historyStream=[IO.File]::OpenRead($path)
    $historyHasher=[Security.Cryptography.SHA256]::Create()
    try {$historySha256=[BitConverter]::ToString($historyHasher.ComputeHash($historyStream)).Replace('-','')}
    finally {$historyHasher.Dispose();$historyStream.Dispose()}
    return @{schema=$descriptor.schema;path=$path;relativePath=$expected;recordCount=$descriptor.recordCount;bytes=$file.Length;sha256=$historySha256}
}

switch ($Action) {
    "Start" {
        $sessionLifecycleLock = $null
        try {
            $sessionLifecycleLock = Open-SessionLifecycleLock
            Start-AgentSession
        }
        finally {
            if ($null -ne $sessionLifecycleLock) {
                $sessionLifecycleLock.Dispose()
            }
        }
    }
    "Call" {
        $owner = Read-ActiveSession
        Assert-SessionOwnerTag -Owner $owner
        if ([string]$owner.state -ne "ready") {
            throw "The Agent session is not ready (state=$($owner.state))."
        }

        $callDeadlineUtc = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $captureClientLock = $null
        try {
            if ($Method -in @("capture.frame", "capture.sequence")) {
                $captureLockPath = Join-Path ([string]$owner.runDirectory) ".capture-client.lock"
                try {
                    $captureClientLock = [System.IO.File]::Open(
                        $captureLockPath,
                        [System.IO.FileMode]::OpenOrCreate,
                        [System.IO.FileAccess]::ReadWrite,
                        [System.IO.FileShare]::None)
                }
                catch {
                    throw "Another capture.frame call is still waiting for completion."
                }
            }

            $response = Invoke-AgentCall -Owner $owner -CallMethod $Method `
                -CallParamsJson $ParamsJson -Quiet -DeadlineUtc $callDeadlineUtc
        if (-not [bool]$response.ok) {
            Write-Output ($response | ConvertTo-Json -Depth 32 -Compress)
            exit 2
        }

        if ($Method -eq "runtime.step") {
            $targetCompletedSteps = [long]$response.result.targetCompletedSteps
            do {
                $statusResponse = Invoke-AgentCall -Owner $owner -CallMethod "session.status" `
                    -CallParamsJson "{}" -Quiet -DeadlineUtc $callDeadlineUtc
                if (-not [bool]$statusResponse.ok) {
                    Write-Output ($statusResponse | ConvertTo-Json -Depth 32 -Compress)
                    exit 2
                }
                $completedSteps = [long]$statusResponse.result.world.runtimeControl.completedSteps
                if ($completedSteps -ge $targetCompletedSteps) {
                    $result = [ordered]@{
                        ticket = [string]$response.result.ticket
                        targetCompletedSteps = $targetCompletedSteps
                        status = $statusResponse.result
                    }
                    $response = [ordered]@{
                        protocol = 1
                        id = [string]$response.id
                        ok = $true
                        result = $result
                    }
                    break
                }
                if ([DateTime]::UtcNow -ge $callDeadlineUtc) {
                    throw "runtime.step did not reach its completion barrier within $TimeoutSeconds seconds."
                }
                Start-Sleep -Milliseconds 25
            } while ($true)
        }
        elseif ($Method -in @("capture.frame", "capture.sequence")) {
            $captureTicket = [string]$response.result.ticket
            $targetSettledCaptures = [long]$response.result.targetSettledCaptures
            do {
                $statusResponse = Invoke-AgentCall -Owner $owner -CallMethod "session.status" `
                    -CallParamsJson "{}" -Quiet -DeadlineUtc $callDeadlineUtc
                if (-not [bool]$statusResponse.ok) {
                    Write-Output ($statusResponse | ConvertTo-Json -Depth 32 -Compress)
                    exit 2
                }

                $captureStatus = $statusResponse.result.capture
                if ([long]$captureStatus.settledCaptures -ge $targetSettledCaptures) {
                    if ([string]$captureStatus.latest.ticket -ne $captureTicket) {
                        throw "capture.frame completion ticket did not match the request."
                    }
                    $captureState = [string]$captureStatus.latest.state
                    if ($captureState -eq "failed") {
                        $failure = [ordered]@{
                            protocol = 1
                            id = [string]$response.id
                            ok = $false
                            error = [ordered]@{
                                code = [string]$captureStatus.latest.error
                                message = "Live frame capture failed."
                            }
                        }
                        if($Method -eq 'capture.sequence'){
                            $failure.capture=$captureStatus.latest
                            if($null -ne $captureStatus.latest.sequenceHistoryArtifact){$failure.sequenceHistoryArtifact=Resolve-SequenceHistoryArtifact $captureStatus.latest $owner}
                        }
                        Write-Output ($failure | ConvertTo-Json -Depth 32 -Compress)
                        exit 2
                    }
                    if ($captureState -ne "completed") {
                        throw "capture.frame settled in an invalid state: $captureState"
                    }

                    if ($Method -eq 'capture.sequence') {
                        $frames=@($captureStatus.latest.frames)
                        if($frames.Count -lt 8 -or $frames.Count -gt 16){throw 'Capture sequence returned an invalid frame count.'}
                        $lastFrame=$null; $lastStep=$null
                        foreach($frame in $frames) {
                            $absolutePath=[IO.Path]::GetFullPath((Join-Path ([string]$owner.sessionDirectory) ([string]$frame.relativePath)))
                            $captureRoot=[IO.Path]::GetFullPath((Join-Path ([string]$owner.sessionDirectory) 'captures'))+[IO.Path]::DirectorySeparatorChar
                            if(!$absolutePath.StartsWith($captureRoot,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $absolutePath -PathType Leaf)){throw 'Sequence artifact escaped capture root or is missing.'}
                            if($null -ne $lastFrame -and [long]$frame.renderFrame -ne $lastFrame+1){throw 'Capture sequence did not record consecutive frames.'}
                            if($null -ne $lastStep -and [long]$frame.simulationStep -ne $lastStep+1){throw 'Capture sequence did not pair one simulation step with each image.'}
                            $lastFrame=[long]$frame.renderFrame
                            $lastStep=[long]$frame.simulationStep
                            $frame | Add-Member -NotePropertyName path -NotePropertyValue $absolutePath
                        }
                        $sequenceResult=@{ticket=$captureTicket;state=$captureState;frames=$frames}
                        if($null -ne $captureStatus.latest.sequenceHistoryArtifact){$sequenceResult.sequenceHistoryArtifact=Resolve-SequenceHistoryArtifact $captureStatus.latest $owner}
                        $response=[ordered]@{protocol=1;id=[string]$response.id;ok=$true;result=$sequenceResult}
                        break
                    }
                    $relativePath = [string]$captureStatus.latest.relativePath
                    $absolutePath = [System.IO.Path]::GetFullPath(
                        (Join-Path ([string]$owner.sessionDirectory) $relativePath))
                    if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf)) {
                        throw "capture.frame reported completion but no PNG exists: $absolutePath"
                    }
                    $response = [ordered]@{
                        protocol = 1
                        id = [string]$response.id
                        ok = $true
                        result = [ordered]@{
                            ticket = $captureTicket
                            state = $captureState
                            renderFrame = [long]$captureStatus.latest.settledRenderFrame
                            renderFrameClock = "SimulationClock at asynchronous capture settlement (legacy alias)"
                            settledRenderFrame = [long]$captureStatus.latest.settledRenderFrame
                            recordedIdentity = $captureStatus.latest.recordedIdentity
                            path = $absolutePath
                        }
                    }
                    break
                }
                if ([DateTime]::UtcNow -ge $callDeadlineUtc) {
                    if($Method -eq 'capture.sequence') {
                        $null=Invoke-AgentCall -Owner $owner -CallMethod 'capture.cancel' -CallParamsJson '{}' -Quiet -DeadlineUtc ([DateTime]::UtcNow.AddSeconds(5))
                    }
                    throw "capture.frame did not reach its completion barrier within $TimeoutSeconds seconds."
                }
                Start-Sleep -Milliseconds 25
            } while ($true)
        }
        Write-Output ($response | ConvertTo-Json -Depth 32 -Compress)
        }
        finally {
            if ($null -ne $captureClientLock) {
                $captureClientLock.Dispose()
            }
        }
    }
    "Status" {
        $sessionLifecycleLock = $null
        try {
            $sessionLifecycleLock = Open-SessionLifecycleLock
            $owner = Read-ActiveSession
            Assert-SessionOwnerTag -Owner $owner
            if ($null -eq (Get-RecordedProcess -Owner $owner)) {
                [void](Stop-RecordedSessionJob -Owner $owner)
                Set-RecordedSessionState -Owner $owner -State "stopped"
                Write-Output (([ordered]@{
                    protocolVersion = 1
                    state = "stopped"
                    pid = [int]$owner.pid
                    sessionDirectory = [string]$owner.sessionDirectory
                }) | ConvertTo-Json -Compress)
                break
            }
            $statusPath = Join-Path ([string]$owner.sessionDirectory) "status.json"
            $status = Read-JsonFileWithRetry -Path $statusPath
            Write-Output $status.Json
        }
        finally {
            if ($null -ne $sessionLifecycleLock) {
                $sessionLifecycleLock.Dispose()
            }
        }
    }
    "Stop" {
        $sessionLifecycleLock = $null
        try {
            $sessionLifecycleLock = Open-SessionLifecycleLock
            $owner = Read-ActiveSession
            Assert-SessionOwnerTag -Owner $owner
            Stop-AgentSession -Owner $owner
        }
        finally {
            if ($null -ne $sessionLifecycleLock) {
                $sessionLifecycleLock.Dispose()
            }
        }
    }
}

if ($Action -eq 'Stop') { Invoke-HEStorageAuto }
