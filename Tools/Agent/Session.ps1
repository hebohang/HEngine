param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Start", "Call", "Status", "Stop")]
    [string]$Action,

    [string]$Method = "session.status",

    [string]$ParamsJson = "{}",

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [ValidateRange(1, 300)]
    [int]$TimeoutSeconds = 30,

    [double]$FixedDeltaSeconds = 0.0166667,

    [string]$Resolution = "1280x720",

    [switch]$RenderValidation,

    [switch]$RenderStats,

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

function Write-JsonAtomically {
    param(
        [string]$Path,
        [string]$Contents
    )

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $temporary = "$Path.tmp.$PID.$([guid]::NewGuid().ToString('N'))"
    try {
        $encoding = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($temporary, $Contents, $encoding)
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
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

function Get-ProjectExecutableName {
    $descriptorPath = Join-Path $script:repositoryRoot "Project\Project.heproject"
    if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
        throw "Project descriptor was not found: $descriptorPath"
    }

    $descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $descriptorPath |
        ConvertFrom-Json
    $executableName = [string]$descriptor.ExecutableName
    if ([string]::IsNullOrWhiteSpace($executableName) -or
        $executableName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
        $executableName -match '[.][Ee][Xx][Ee]$') {
        throw "Project descriptor contains an invalid ExecutableName."
    }
    return $executableName
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
        if (Test-Path -LiteralPath $Path) {
            try {
                $json = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
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
        if (Test-Path -LiteralPath $responsePath) {
            $responseJson = Get-Content -Raw -Encoding UTF8 -LiteralPath $responsePath
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

function Start-AgentSession {
    if (Test-Path -LiteralPath $script:activeSessionPath) {
        $previousOwner = Read-ActiveSession
        if ($null -ne (Get-RecordedProcess -Owner $previousOwner)) {
            throw "An Agent session is already running (pid=$($previousOwner.pid)). Stop it before starting another."
        }
        [void](Stop-RecordedSessionJob -Owner $previousOwner)
    }

    & $script:buildScript -Target HGame -Configuration $Configuration -Parallel $Parallel `
        -EnableTracy:$EnableTracy
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $modeName = if ($EnableTracy) { "game-tracy" } else { "game" }
    $executableName = Get-ProjectExecutableName
    $artifact = Join-Path $script:repositoryRoot "out\agent\$modeName-$($Configuration.ToLowerInvariant())\bin\$executableName.exe"
    if (-not (Test-Path -LiteralPath $artifact)) {
        throw "HGame artifact was not found: $artifact"
    }

    $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), [guid]::NewGuid().ToString("N").Substring(0, 12)
    $runDirectory = Join-Path $script:sessionsRoot $runId
    $sessionDirectory = Join-Path $runDirectory "mailbox"
    New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
    $stdoutLog = Join-Path $runDirectory "game.stdout.log"
    $stderrLog = Join-Path $runDirectory "game.stderr.log"
    $savedRoot = Join-Path $runDirectory "Saved"
    New-Item -ItemType Directory -Force -Path $savedRoot | Out-Null
    $token = New-SessionToken
    $jobName = "Local\HEngineAgentSession_$([guid]::NewGuid().ToString('N'))"
    $fixedDeltaText = $FixedDeltaSeconds.ToString("R", [System.Globalization.CultureInfo]::InvariantCulture)
    $arguments = @(
        "--agent-session=`"$sessionDirectory`"",
        "--agent-token=$token",
        "--agent-start-paused",
        "--saved-root=`"$savedRoot`"",
        "--fixed-delta=$fixedDeltaText",
        "--resolution=$Resolution",
        "--no-cursor-capture",
        "--no-vsync"
    )
    if ($RenderValidation) {
        $arguments += "--render-validation"
    }
    if ($RenderStats) {
        $arguments += "--render-stats"
    }

    $executionLockPath = Join-Path (Split-Path -Parent (Split-Path -Parent $artifact)) ".agent-execution.lock"
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
            throw "Another Agent build or owned run is using the HGame directory. Wait for it to finish."
        }

        $runningArtifact = Get-AgentArtifactProcess `
            -ArtifactPath $artifact -ProcessName $executableName
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
            $owner = [ordered]@{
                protocolVersion = 1
                state = "starting"
                runDirectory = $runDirectory
                sessionDirectory = $sessionDirectory
                token = $token
                pid = $process.Id
                processStartUtcTicks = $process.StartTime.ToUniversalTime().Ticks
                jobName = $jobName
                artifact = $artifact
                tracyEnabled = [bool]$EnableTracy
                renderValidation = [bool]$RenderValidation
                renderStats = [bool]$RenderStats
                stdoutLog = $stdoutLog
                stderrLog = $stderrLog
            }
            if (-not [string]::IsNullOrEmpty($OwnerTag)) {
                $owner["ownerTag"] = $OwnerTag.ToLowerInvariant()
            }
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
        Write-Host "agent session: already stopped"
        return
    }

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

    if (-not $process.WaitForExit(5000)) {
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
}

if ([double]::IsNaN($FixedDeltaSeconds) -or
    [double]::IsInfinity($FixedDeltaSeconds) -or
    $FixedDeltaSeconds -le 0.0 -or $FixedDeltaSeconds -gt 1.0) {
    throw "-FixedDeltaSeconds must be in the range (0, 1]."
}
if ($Resolution -notmatch "^[1-9][0-9]{0,3}[xX][1-9][0-9]{0,3}$") {
    throw "-Resolution must use WIDTHxHEIGHT."
}
$resolutionParts = $Resolution -split "[xX]"
if ([int]$resolutionParts[0] -gt 8192 -or [int]$resolutionParts[1] -gt 8192) {
    throw "-Resolution dimensions must be in the range 1..8192."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildScript = Join-Path $PSScriptRoot "Build.ps1"
$sessionsRoot = Join-Path $repositoryRoot "out\agent\sessions"
$activeSessionPath = Join-Path $sessionsRoot "active.json"
$sessionLifecycleLockPath = Join-Path $sessionsRoot ".agent-session-lifecycle.lock"
New-Item -ItemType Directory -Force -Path $sessionsRoot | Out-Null

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
            if ($Method -eq "capture.frame") {
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
        elseif ($Method -eq "capture.frame") {
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
                        Write-Output ($failure | ConvertTo-Json -Depth 16 -Compress)
                        exit 2
                    }
                    if ($captureState -ne "completed") {
                        throw "capture.frame settled in an invalid state: $captureState"
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
                            path = $absolutePath
                        }
                    }
                    break
                }
                if ([DateTime]::UtcNow -ge $callDeadlineUtc) {
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
