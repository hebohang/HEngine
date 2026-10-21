[CmdletBinding()]
param(
    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "RelWithDebInfo",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [ValidateRange(1, 300)]
    [int]$AgentTimeoutSeconds = 300,

    [ValidateRange(1, 3600)]
    [int]$BuildTimeoutSeconds = 900,

    [ValidateRange(1, 120)]
    [int]$CaptureConnectTimeoutSeconds = 30,

    [ValidateRange(1, 300)]
    [int]$CaptureShutdownTimeoutSeconds = 60,

    [ValidateRange(1, 600)]
    [int]$ExportTimeoutSeconds = 300,

    [double]$FixedDeltaSeconds = 0.0166667,

    [string]$Resolution = "1280x720",

    [double]$StartClickX = 640.0,

    [double]$StartClickY = 410.0,

    [string]$StartupScene = "",

    [ValidateRange(0, 20)]
    [int]$WarmupBatches = 4,

    [ValidateRange(1, 60)]
    [int]$CaptureBatches = 15,

    [ValidateNotNullOrEmpty()]
    [string]$FrameZoneName = "Application::Frame",

    [ValidateNotNullOrEmpty()]
    [string]$GameplayZoneName = "SurvivorGame::GameplayFrame",

    [ValidateNotNullOrEmpty()]
    [string]$PhysicsZoneName = "class HEngine::PhysicSystem3D",

    [ValidateNotNullOrEmpty()]
    [string]$PythonZoneName = "class HEngine::PythonScriptSystem",

    [ValidateNotNullOrEmpty()]
    [string]$AnimationZoneName = "class HEngine::AnimationSystem",

    [ValidateNotNullOrEmpty()]
    [string]$RenderZoneName = "class HEngine::RenderSystem3D",

    [ValidateNotNullOrEmpty()]
    [string]$PresentZoneName = "Present",

    [string]$AnalyzeTracePath = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Process.ps1")

$script:repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$script:hectlPath = Join-Path $script:repositoryRoot "hectl.bat"
$script:hectlScriptPath = Join-Path $PSScriptRoot "hectl.ps1"
$script:sessionScriptPath = Join-Path $PSScriptRoot "Session.ps1"
$script:buildScriptPath = Join-Path $PSScriptRoot "Build.ps1"
$script:profilesRoot = Join-Path $script:repositoryRoot "out\agent\profiles"
$script:activeSessionPath = Join-Path $script:repositoryRoot "out\agent\sessions\active.json"
$script:hectlLogPath = $null
$script:hectlCallIndex = 0
$script:sessionCallIndex = 0
$script:toolJob = $null
$script:ownerTag = $null

# Keep this recipe fixed so profile runs remain directly comparable.
$script:warmupBatches = $WarmupBatches
$script:captureBatches = $CaptureBatches
$script:framesPerBatch = 120
$script:expectedActiveFrames = $script:captureBatches * $script:framesPerBatch
$script:tracyAddress = "127.0.0.1"
$script:tracyPort = 8086
$script:tracyVersion = "0.13.1"
$script:processTimeoutBufferSeconds = 30
$script:analysisRevision = "gameplay-selection-v4-python-phases"
$script:percentileMethod = "Type 7 linear interpolation (h = (n - 1) * p)"
$script:survivorUpdateZoneName = "SurvivorGame::Update"
$script:survivorPhaseZoneNames = [ordered]@{
    survivorBookkeeping = "SurvivorGame::Phase::Bookkeeping"
    survivorPendingPickupSpawns = "SurvivorGame::Phase::PendingPickupSpawns"
    survivorWaveSpawn = "SurvivorGame::Phase::WaveSpawn"
    survivorMonsters = "SurvivorGame::Phase::Monsters"
    survivorPickups = "SurvivorGame::Phase::Pickups"
    survivorWeapons = "SurvivorGame::Phase::Weapons"
    survivorHud = "SurvivorGame::Phase::Hud"
}
$script:survivorPhaseChildKeys = @($script:survivorPhaseZoneNames.Keys)
$script:survivorProfileZoneKeys = @("survivorUpdate") + $script:survivorPhaseChildKeys

function Assert-NewFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to overwrite an existing profile artifact: $Path"
    }
}

function Write-NewUtf8Text {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Contents
    )

    Assert-NewFile -Path $Path
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Contents, $encoding)
}

function Write-NewJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    Write-NewUtf8Text -Path $Path -Contents ($Value | ConvertTo-Json -Depth 16)
}

function Get-FileFingerprint {
    param([Parameter(Mandatory = $true)][string]$Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $item.FullName
        length = [long]$item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-OptionalFileProvenance {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{
            path = $Path
            found = $false
        }
    }
    $fingerprint = Get-FileFingerprint -Path $Path
    $result = [ordered]@{
        path = $fingerprint.path
        found = $true
        length = $fingerprint.length
        sha256 = $fingerprint.sha256
    }
    return $result
}

function Get-GitProvenance {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $result = [ordered]@{
        available = $false
    }
    try {
        $commitOutput = @(& git -C $RepositoryRoot rev-parse HEAD 2>$null)
        $commitExitCode = $LASTEXITCODE
        if ($commitExitCode -ne 0 -or $commitOutput.Count -ne 1) {
            $result["error"] = "git rev-parse failed with exit code $commitExitCode."
            return $result
        }

        $statusLines = @(& git -C $RepositoryRoot status `
            --porcelain=v1 --untracked-files=all 2>$null)
        $statusExitCode = $LASTEXITCODE
        if ($statusExitCode -ne 0) {
            $result["error"] = "git status failed with exit code $statusExitCode."
            return $result
        }

        $result["available"] = $true
        $result["commit"] = [string]$commitOutput[0]
        $result["dirty"] = $statusLines.Count -gt 0
        $result["dirtyEntryCount"] = $statusLines.Count
        $statusText = [string]::Join("`n", [string[]]$statusLines)
        $statusBytes = [System.Text.Encoding]::UTF8.GetBytes($statusText)
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $result["statusSha256"] = ([BitConverter]::ToString(
                $sha256.ComputeHash($statusBytes))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $sha256.Dispose()
        }
        return $result
    }
    catch {
        $result["error"] = $_.Exception.Message
        return $result
    }
}

function Get-SourceTraceProvenance {
    param([Parameter(Mandatory = $true)][string]$TracePath)

    $trace = Get-FileFingerprint -Path $TracePath
    $sidecarPath = Join-Path (Split-Path -Parent $TracePath) "metadata.json"
    $sidecar = [ordered]@{
        found = $false
        trustStatus = "untrusted"
        trustReason = "Adjacent metadata.json was not found."
    }
    if (Test-Path -LiteralPath $sidecarPath -PathType Leaf) {
        $sidecar["found"] = $true
        $sidecarFingerprint = Get-FileFingerprint -Path $sidecarPath
        foreach ($name in $sidecarFingerprint.Keys) {
            $sidecar[$name] = $sidecarFingerprint[$name]
        }
        try {
            $sourceMetadata = Get-Content -Raw -LiteralPath $sidecarPath | ConvertFrom-Json
            $sidecar["parseStatus"] = "pass"
            $document = [ordered]@{}
            foreach ($name in @(
                "schemaVersion",
                "status",
                "error",
                "mode",
                "startedUtc",
                "completedUtc",
                "analysisRevision")) {
                if ($null -ne $sourceMetadata.PSObject.Properties[$name]) {
                    $document[$name] = $sourceMetadata.$name
                }
            }
            $sidecar["document"] = $document

            $capturedTraceProperty = $sourceMetadata.PSObject.Properties["capturedTrace"]
            if ($null -eq $capturedTraceProperty) {
                $sidecar["trustReason"] = "Adjacent metadata does not contain a capturedTrace fingerprint."
            }
            else {
                $declaredCapturedTrace = $sourceMetadata.capturedTrace
                [long]$declaredLength = 0
                $bValidLength = [long]::TryParse(
                    [string]$declaredCapturedTrace.length,
                    [System.Globalization.NumberStyles]::Integer,
                    [System.Globalization.CultureInfo]::InvariantCulture,
                    [ref]$declaredLength)
                $declaredSha256 = ([string]$declaredCapturedTrace.sha256).ToLowerInvariant()
                $sidecar["declaredCapturedTrace"] = [ordered]@{
                    path = [string]$declaredCapturedTrace.path
                    length = if ($bValidLength) { $declaredLength } else { $null }
                    sha256 = $declaredSha256
                }

                if (-not $bValidLength -or $declaredLength -lt 0 -or
                    $declaredSha256 -notmatch '^[0-9a-f]{64}$') {
                    $sidecar["trustReason"] = "Adjacent metadata contains an invalid capturedTrace fingerprint."
                }
                elseif ($declaredLength -ne $trace.length -or
                    -not [string]::Equals(
                        $declaredSha256,
                        $trace.sha256,
                        [System.StringComparison]::Ordinal)) {
                    $sidecar["trustReason"] = "Adjacent metadata capturedTrace length/SHA-256 does not match the analyzed trace."
                }
                else {
                    $sidecar["trustStatus"] = "verified"
                    $sidecar["trustReason"] = "Adjacent metadata capturedTrace length/SHA-256 matches the analyzed trace."
                    $trustedCaptureMetadata = [ordered]@{}
                    foreach ($name in @("captureConfiguration", "recipe", "session", "profiledScript")) {
                        if ($null -ne $sourceMetadata.PSObject.Properties[$name]) {
                            $trustedCaptureMetadata[$name] = $sourceMetadata.$name
                        }
                    }
                    $sidecar["trustedCaptureMetadata"] = $trustedCaptureMetadata
                }
            }
        }
        catch {
            $sidecar["parseStatus"] = "fail"
            $sidecar["parseError"] = $_.Exception.Message
            $sidecar["trustStatus"] = "untrusted"
            $sidecar["trustReason"] = "Adjacent metadata could not be parsed."
        }
    }

    return [ordered]@{
        trace = $trace
        sidecar = $sidecar
    }
}

function Get-GpuCsvManifest {
    param([Parameter(Mandatory = $true)][string]$CsvPath)

    $expectedHeader = "name,src_file,Time from start of program,GPU execution time"
    $reader = [System.IO.File]::OpenText($CsvPath)
    [long]$rowCount = 0
    try {
        $actualHeader = $reader.ReadLine()
        if (-not [string]::Equals(
            $actualHeader,
            $expectedHeader,
            [System.StringComparison]::Ordinal)) {
            throw "Unexpected Tracy $($script:tracyVersion) GPU CSV header. Expected '$expectedHeader', got '$actualHeader'."
        }

        while (($line = $reader.ReadLine()) -ne $null) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                throw "GPU CSV contains an empty data row at line $($rowCount + 2): $CsvPath"
            }
            ++$rowCount
        }
    }
    finally {
        $reader.Dispose()
    }

    $fingerprint = Get-FileFingerprint -Path $CsvPath
    return [ordered]@{
        path = $fingerprint.path
        length = $fingerprint.length
        sha256 = $fingerprint.sha256
        header = $expectedHeader
        rowCount = $rowCount
        classification = "raw-unfiltered"
        scope = "Entire Tracy trace; events are not associated with selected active CPU frames."
        conclusion = "No GPU frame-time, percentile, or CPU/GPU bottleneck conclusion is produced."
    }
}

function ConvertTo-NativeArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value.IndexOf('"') -ge 0) {
        throw "Native tool arguments cannot contain a double quote: $Value"
    }
    if ($Value.Length -eq 0 -or $Value -match '\s') {
        return '"' + $Value + '"'
    }
    return $Value
}

function Get-LogTail {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$Count = 40
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "<missing log: $Path>"
    }
    return ((Get-Content -LiteralPath $Path -Tail $Count -ErrorAction SilentlyContinue) -join [Environment]::NewLine)
}

function Invoke-HeCtl {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][ValidateRange(1, 7200)][int]$ProcessTimeoutSeconds
    )

    ++$script:hectlCallIndex
    $callName = "hectl-call-{0:d3}" -f $script:hectlCallIndex
    $callDirectory = Split-Path -Parent $script:hectlLogPath
    $stdoutPath = Join-Path $callDirectory "$callName.stdout.log"
    $stderrPath = Join-Path $callDirectory "$callName.stderr.log"
    Assert-NewFile -Path $stdoutPath
    Assert-NewFile -Path $stderrPath

    $nativeArguments = @(
        "-NoLogo",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (ConvertTo-NativeArgument -Value $script:hectlScriptPath)
    )
    $nativeArguments += @($Arguments | ForEach-Object {
        ConvertTo-NativeArgument -Value ([string]$_)
    })

    $startedAt = Get-Date
    $process = $null
    $exitCode = $null
    $failure = $null
    $terminationFailure = $null
    try {
        $process = Start-Process -FilePath "powershell.exe" -ArgumentList $nativeArguments `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath `
            -WindowStyle Hidden -PassThru
        # Cache a real process handle before it can exit so Windows PowerShell
        # reliably exposes ExitCode. Redirecting to files also prevents the
        # long-lived game process from keeping an in-memory pipeline open.
        $null = $process.Handle
        # Keep the hectl wrapper and descendants in the profile Job. HGame also
        # retains its named Session Job for cross-call cleanup.
        Add-AgentProcessToJob -Job $script:toolJob -Process $process
        if (-not $process.WaitForExit($ProcessTimeoutSeconds * 1000)) {
            Stop-AgentOwnedProcess -Process $process -Description $Label
            throw "$Label did not exit within its $ProcessTimeoutSeconds second process budget."
        }
        $process.WaitForExit()
        $exitCode = [int]$process.ExitCode
    }
    catch {
        $failure = $_
        if ($null -ne $process) {
            try {
                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-AgentOwnedProcess -Process $process -Description $Label
                }
            }
            catch {
                $terminationFailure = $_.Exception.Message
            }
        }
    }

    $stdout = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    $stderr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    $output = @($stdout) + @($stderr)
    $elapsed = (Get-Date) - $startedAt
    if ($null -eq $failure -and $exitCode -ne 0) {
        $tail = @($output | Select-Object -Last 40 | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        try {
            throw "$Label failed with exit code $exitCode.$([Environment]::NewLine)$tail"
        }
        catch {
            $failure = $_
        }
    }
    $logLines = @(
        "[{0:o}] {1}" -f $startedAt.ToUniversalTime(), $Label
        "command: hectl.bat {0}" -f ($Arguments -join " ")
        "processBudgetSeconds: {0}; exit: {1}; elapsedSeconds: {2:n3}" -f `
            $ProcessTimeoutSeconds,
            $(if ($null -eq $exitCode) { "unknown" } else { $exitCode }),
            $elapsed.TotalSeconds
    )
    $logLines += @($output | ForEach-Object { [string]$_ })
    if ($null -ne $terminationFailure) {
        $logLines += "termination failure: $terminationFailure"
    }
    $logLines += ""
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::AppendAllText(
        $script:hectlLogPath,
        ($logLines -join [Environment]::NewLine) + [Environment]::NewLine,
        $encoding)

    if ($null -ne $failure) {
        throw $failure
    }
    if ($null -ne $terminationFailure) {
        throw "$Label cleanup failed after process completion: $terminationFailure"
    }
    Write-Host ("{0}: PASS ({1:n1}s)" -f $Label, $elapsed.TotalSeconds)
    return $output
}

function Invoke-AgentSession {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][ValidateSet("Call", "Status", "Stop")][string]$Action,
        [string]$Method = "session.status",
        [string]$ParamsJson = "{}"
    )

    ++$script:sessionCallIndex
    $callName = "agent-session-call-{0:d3}" -f $script:sessionCallIndex
    $callDirectory = Split-Path -Parent $script:hectlLogPath
    $stdoutPath = Join-Path $callDirectory "$callName.stdout.log"
    $stderrPath = Join-Path $callDirectory "$callName.stderr.log"

    if ([string]$script:ownerTag -notmatch '^[0-9a-f]{32}$') {
        throw "The Tracy profile ownerTag is not initialized for an Agent session call."
    }
    $payload = [ordered]@{
        scriptPath = $script:sessionScriptPath
        action = $Action
        method = $Method
        paramsJson = $ParamsJson
        timeoutSeconds = $AgentTimeoutSeconds
        ownerTag = $script:ownerTag
    } | ConvertTo-Json -Compress
    $payloadBase64 = [Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes($payload))
    $childScript = @"
`$ErrorActionPreference = "Stop"
`$payloadJson = [System.Text.Encoding]::UTF8.GetString(
    [Convert]::FromBase64String("$payloadBase64"))
`$payload = `$payloadJson | ConvertFrom-Json
try {
    & ([string]`$payload.scriptPath) ``
        -Action ([string]`$payload.action) ``
        -Method ([string]`$payload.method) ``
        -ParamsJson ([string]`$payload.paramsJson) ``
        -TimeoutSeconds ([int]`$payload.timeoutSeconds) ``
        -OwnerTag ([string]`$payload.ownerTag)
    exit 0
}
catch {
    [Console]::Error.WriteLine(`$_.Exception.Message)
    if (-not [string]::IsNullOrWhiteSpace(`$_.ScriptStackTrace)) {
        [Console]::Error.WriteLine(`$_.ScriptStackTrace)
    }
    exit 1
}
"@
    $encodedCommand = [Convert]::ToBase64String(
        [System.Text.Encoding]::Unicode.GetBytes($childScript))

    $startedAt = Get-Date
    $process = Start-OwnedProcess -FilePath "powershell.exe" `
        -Arguments @(
            "-NoLogo",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-EncodedCommand",
            $encodedCommand) `
        -StdoutPath $stdoutPath -StderrPath $stderrPath
    $failure = $null
    $exitCode = $null
    try {
        $processTimeoutSeconds = $AgentTimeoutSeconds + $script:processTimeoutBufferSeconds
        if (-not $process.WaitForExit($processTimeoutSeconds * 1000)) {
            Stop-AgentOwnedProcess -Process $process -Description $Label
            throw "$Label did not exit within $processTimeoutSeconds seconds."
        }
        $process.WaitForExit()
        $exitCode = [int]$process.ExitCode
        if ($exitCode -ne 0) {
            $stderrTail = Get-LogTail -Path $stderrPath
            $stdoutTail = Get-LogTail -Path $stdoutPath
            throw "$Label failed with exit code $exitCode.`nstdout:`n$stdoutTail`nstderr:`n$stderrTail"
        }
    }
    catch {
        $failure = $_
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-AgentOwnedProcess -Process $process -Description $Label
        }
    }

    $stdout = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    $stderr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    $output = @($stdout) + @($stderr)
    $elapsed = (Get-Date) - $startedAt
    $logLines = @(
        "[{0:o}] {1}" -f $startedAt.ToUniversalTime(), $Label
        "command: Session.ps1 -Action {0} -Method {1}" -f $Action, $Method
        "exit: {0}; elapsedSeconds: {1:n3}" -f `
            $(if ($null -eq $exitCode) { "unknown" } else { $exitCode }),
            $elapsed.TotalSeconds
    )
    $logLines += @($output | ForEach-Object { [string]$_ })
    $logLines += ""
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::AppendAllText(
        $script:hectlLogPath,
        ($logLines -join [Environment]::NewLine) + [Environment]::NewLine,
        $encoding)

    if ($null -ne $failure) {
        throw $failure
    }
    Write-Host ("{0}: PASS ({1:n1}s)" -f $Label, $elapsed.TotalSeconds)
    return $output
}

function Start-OwnedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )

    Assert-NewFile -Path $StdoutPath
    Assert-NewFile -Path $StderrPath
    $nativeArguments = @($Arguments | ForEach-Object {
        ConvertTo-NativeArgument -Value ([string]$_)
    })
    $process = Start-Process -FilePath $FilePath -ArgumentList $nativeArguments `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath `
        -WindowStyle Hidden -PassThru
    try {
        $null = $process.Handle
        Add-AgentProcessToJob -Job $script:toolJob -Process $process
        return $process
    }
    catch {
        try {
            Stop-AgentOwnedProcess -Process $process -Description ([System.IO.Path]::GetFileName($FilePath))
        }
        catch {
        }
        throw
    }
}

function ConvertTo-CanonicalIPv4Address {
    param([Parameter(Mandatory = $true)][string]$Address)

    [System.Net.IPAddress]$parsed = $null
    if (-not [System.Net.IPAddress]::TryParse($Address, [ref]$parsed)) {
        return $null
    }
    if ($parsed.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork) {
        return $parsed.ToString()
    }
    if ($parsed.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6 -and
        $parsed.IsIPv4MappedToIPv6) {
        return $parsed.MapToIPv4().ToString()
    }
    return $null
}

function ConvertTo-NormalizedTcpTuple {
    param([Parameter(Mandatory = $true)]$Connection)

    $localAddress = ConvertTo-CanonicalIPv4Address -Address ([string]$Connection.LocalAddress)
    $remoteAddress = ConvertTo-CanonicalIPv4Address -Address ([string]$Connection.RemoteAddress)
    if ($null -eq $localAddress -or $null -eq $remoteAddress) {
        return $null
    }
    return [pscustomobject]@{
        LocalAddress = $localAddress
        LocalPort = [int]$Connection.LocalPort
        RemoteAddress = $remoteAddress
        RemotePort = [int]$Connection.RemotePort
    }
}

function Get-TracyConnectionPairs {
    param(
        [Parameter(Mandatory = $true)][int]$CapturePid,
        [Parameter(Mandatory = $true)][int]$GamePid
    )

    $captureConnections = @(Get-NetTCPConnection -OwningProcess $CapturePid `
        -State Established -ErrorAction SilentlyContinue)
    $gameConnections = @(Get-NetTCPConnection -OwningProcess $GamePid `
        -State Established -ErrorAction SilentlyContinue)
    $pairs = New-Object 'System.Collections.Generic.List[object]'
    foreach ($captureConnection in $captureConnections) {
        $captureTuple = ConvertTo-NormalizedTcpTuple -Connection $captureConnection
        if ($null -eq $captureTuple -or
            $captureTuple.RemotePort -ne $script:tracyPort -or
            -not [string]::Equals(
                $captureTuple.RemoteAddress,
                $script:tracyAddress,
                [System.StringComparison]::Ordinal)) {
            continue
        }
        foreach ($gameConnection in $gameConnections) {
            $gameTuple = ConvertTo-NormalizedTcpTuple -Connection $gameConnection
            if ($null -eq $gameTuple) {
                continue
            }
            if ($captureTuple.LocalPort -eq $gameTuple.RemotePort -and
                $captureTuple.RemotePort -eq $gameTuple.LocalPort -and
                [string]::Equals(
                    $captureTuple.LocalAddress,
                    $gameTuple.RemoteAddress,
                    [System.StringComparison]::Ordinal) -and
                [string]::Equals(
                    $captureTuple.RemoteAddress,
                    $gameTuple.LocalAddress,
                    [System.StringComparison]::Ordinal)) {
                $pairs.Add([pscustomobject]@{
                    Capture = $captureTuple
                    Game = $gameTuple
                })
            }
        }
    }
    return $pairs.ToArray()
}

function Test-TcpPairEquals {
    param(
        [Parameter(Mandatory = $true)]$Left,
        [Parameter(Mandatory = $true)]$Right
    )

    return $Left.Capture.LocalPort -eq $Right.Capture.LocalPort -and
        $Left.Capture.RemotePort -eq $Right.Capture.RemotePort -and
        $Left.Game.LocalPort -eq $Right.Game.LocalPort -and
        $Left.Game.RemotePort -eq $Right.Game.RemotePort -and
        [string]::Equals($Left.Capture.LocalAddress, $Right.Capture.LocalAddress,
            [System.StringComparison]::Ordinal) -and
        [string]::Equals($Left.Capture.RemoteAddress, $Right.Capture.RemoteAddress,
            [System.StringComparison]::Ordinal) -and
        [string]::Equals($Left.Game.LocalAddress, $Right.Game.LocalAddress,
            [System.StringComparison]::Ordinal) -and
        [string]::Equals($Left.Game.RemoteAddress, $Right.Game.RemoteAddress,
            [System.StringComparison]::Ordinal)
}

function Wait-TracyCaptureConnection {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$GamePid,
        [Parameter(Mandatory = $true)][long]$GameStartUtcTicks,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )

    $getTcpConnection = Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue
    if ($null -eq $getTcpConnection) {
        throw "Get-NetTCPConnection is required to verify that tracy-capture connected before stepping frames."
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($stopwatch.Elapsed.TotalSeconds -lt $CaptureConnectTimeoutSeconds) {
        $Process.Refresh()
        if ($Process.HasExited) {
            $Process.WaitForExit()
            $stdout = Get-LogTail -Path $StdoutPath
            $stderr = Get-LogTail -Path $StderrPath
            throw "tracy-capture exited before connecting (exit=$($Process.ExitCode)).`nstdout:`n$stdout`nstderr:`n$stderr"
        }

        $gameProcess = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
        if ($null -eq $gameProcess -or
            $gameProcess.StartTime.ToUniversalTime().Ticks -ne $GameStartUtcTicks) {
            throw "The profiled HGame PID/start-time identity changed before Tracy connection verification."
        }

        $pairs = @(Get-TracyConnectionPairs -CapturePid $Process.Id -GamePid $GamePid)
        if ($pairs.Count -gt 1) {
            throw "Found multiple symmetric Tracy TCP tuples for capture pid=$($Process.Id) and game pid=$GamePid."
        }
        if ($pairs.Count -eq 1) {
            $firstPair = $pairs[0]
            $firstObservedUtc = [DateTime]::UtcNow
            # TCP establishment precedes the local Tracy protocol handshake.
            Start-Sleep -Milliseconds 1000
            $Process.Refresh()
            $gameProcess = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
            if ($Process.HasExited -or $null -eq $gameProcess -or
                $gameProcess.StartTime.ToUniversalTime().Ticks -ne $GameStartUtcTicks) {
                throw "The capture/game process identity changed during Tracy handshake verification."
            }

            $recheckedPairs = @(Get-TracyConnectionPairs -CapturePid $Process.Id -GamePid $GamePid)
            $samePairs = @($recheckedPairs | Where-Object {
                Test-TcpPairEquals -Left $_ -Right $firstPair
            })
            if ($recheckedPairs.Count -ne 1 -or $samePairs.Count -ne 1) {
                throw "The exact symmetric Tracy TCP tuple was not stable for the one-second handshake dwell."
            }

            Write-Host "tracy-capture: CONNECTED pid=$($Process.Id) gamePid=$GamePid"
            return [ordered]@{
                firstObservedUtc = $firstObservedUtc.ToString("o")
                recheckedUtc = [DateTime]::UtcNow.ToString("o")
                handshakeDwellMilliseconds = 1000
                capturePid = $Process.Id
                captureLocalAddress = $firstPair.Capture.LocalAddress
                captureLocalPort = $firstPair.Capture.LocalPort
                captureRemoteAddress = $firstPair.Capture.RemoteAddress
                captureRemotePort = $firstPair.Capture.RemotePort
                gamePid = $GamePid
                gameProcessStartUtcTicks = $GameStartUtcTicks
                gameLocalAddress = $firstPair.Game.LocalAddress
                gameLocalPort = $firstPair.Game.LocalPort
                gameRemoteAddress = $firstPair.Game.RemoteAddress
                gameRemotePort = $firstPair.Game.RemotePort
            }
        }
        Start-Sleep -Milliseconds 100
    }

    throw "tracy-capture did not establish an exact connection to game pid=$GamePid at $($script:tracyAddress):$($script:tracyPort) within $CaptureConnectTimeoutSeconds seconds."
}

function Wait-OwnedProcess {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )

    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        throw "$Description did not exit within $TimeoutSeconds seconds."
    }
    $Process.WaitForExit()
    if ($Process.ExitCode -ne 0) {
        $stdout = Get-LogTail -Path $StdoutPath
        $stderr = Get-LogTail -Path $StderrPath
        throw "$Description failed with exit code $($Process.ExitCode).`nstdout:`n$stdout`nstderr:`n$stderr"
    }
}

function Invoke-TracyExport {
    param(
        [Parameter(Mandatory = $true)][string]$ExporterPath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$CsvPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $process = Start-OwnedProcess -FilePath $ExporterPath -Arguments $Arguments `
        -StdoutPath $CsvPath -StderrPath $StderrPath
    try {
        Wait-OwnedProcess -Process $process -TimeoutSeconds $ExportTimeoutSeconds `
            -Description $Label -StdoutPath $CsvPath -StderrPath $StderrPath
    }
    catch {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-AgentOwnedProcess -Process $process -Description $Label
        }
        throw
    }
    if (-not (Test-Path -LiteralPath $CsvPath -PathType Leaf) -or
        (Get-Item -LiteralPath $CsvPath).Length -eq 0) {
        throw "$Label succeeded but produced an empty CSV: $CsvPath"
    }
}

function ConvertTo-ZoneInterval {
    param(
        [Parameter(Mandatory = $true)]$Row,
        [Parameter(Mandatory = $true)][string]$CsvPath
    )

    [long]$startNs = 0
    [long]$durationNs = 0
    $numberStyles = [System.Globalization.NumberStyles]::Integer
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    if (-not [long]::TryParse([string]$Row.ns_since_start, $numberStyles, $culture, [ref]$startNs) -or
        -not [long]::TryParse([string]$Row.exec_time_ns, $numberStyles, $culture, [ref]$durationNs)) {
        throw "Invalid Tracy timing value in $CsvPath for zone '$([string]$Row.name)'."
    }
    if ($startNs -lt 0 -or $durationNs -le 0 -or $startNs -gt [long]::MaxValue - $durationNs) {
        throw "Out-of-range Tracy interval in $CsvPath for zone '$([string]$Row.name)'."
    }
    return [pscustomobject]@{
        Name = [string]$Row.name
        Thread = [string]$Row.thread
        StartNs = $startNs
        EndNs = $startNs + $durationNs
        DurationNs = $durationNs
    }
}

function Read-ExactZoneIntervals {
    param(
        [Parameter(Mandatory = $true)][string]$CsvPath,
        [Parameter(Mandatory = $true)][string]$ExactZoneName,
        [Parameter(Mandatory = $true)][string]$DiagnosticSubstring,
        [switch]$AllowEmpty
    )

    $expectedHeader = "name,src_file,src_line,ns_since_start,exec_time_ns,thread,value"
    $actualHeader = Get-Content -LiteralPath $CsvPath -TotalCount 1
    if (-not [string]::Equals($actualHeader, $expectedHeader, [System.StringComparison]::Ordinal)) {
        throw "Unexpected Tracy $($script:tracyVersion) CPU CSV header. Expected '$expectedHeader', got '$actualHeader'."
    }

    $intervals = New-Object 'System.Collections.Generic.List[object]'
    $nearbyNames = @{}
    Import-Csv -LiteralPath $CsvPath | ForEach-Object {
        $name = [string]$_.name
        if ([string]::Equals($name, $ExactZoneName, [System.StringComparison]::Ordinal)) {
            $intervals.Add((ConvertTo-ZoneInterval -Row $_ -CsvPath $CsvPath))
        }
        elseif ($name.IndexOf($DiagnosticSubstring, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $nearbyNames[$name] = $true
        }
    }

    if ($intervals.Count -eq 0 -and -not $AllowEmpty) {
        $nearbyText = if ($nearbyNames.Count -eq 0) {
            "<none>"
        }
        else {
            (@($nearbyNames.Keys | Sort-Object) -join "; ")
        }
        throw "CPU CSV contains no exact '$ExactZoneName' zones. Names containing '$DiagnosticSubstring': $nearbyText"
    }
    return $intervals.ToArray()
}

function Select-ActiveFrames {
    param(
        [Parameter(Mandatory = $true)][string]$FrameCsvPath,
        [Parameter(Mandatory = $true)][string]$SelectionCsvPath,
        [Parameter(Mandatory = $true)][string]$SelectionZoneName
    )

    $frames = @(Read-ExactZoneIntervals -CsvPath $FrameCsvPath `
        -ExactZoneName $FrameZoneName -DiagnosticSubstring $FrameZoneName)
    $activeZones = @(Read-ExactZoneIntervals -CsvPath $SelectionCsvPath `
        -ExactZoneName $SelectionZoneName -DiagnosticSubstring $SelectionZoneName)

    if ($frames.Count -eq 0 -or $activeZones.Count -eq 0) {
        throw "Tracy active-frame inputs unexpectedly contained no intervals."
    }

    if ($activeZones.Count -ne $script:expectedActiveFrames) {
        throw "Expected exactly $($script:expectedActiveFrames) exact '$SelectionZoneName' markers, but found $($activeZones.Count)."
    }

    $sortedFrames = @($frames | Sort-Object StartNs)
    $frameThreads = @($sortedFrames | ForEach-Object { $_.Thread } | Sort-Object -Unique)
    if ($frameThreads.Count -ne 1) {
        throw "Expected '$FrameZoneName' intervals on exactly one thread, but found $($frameThreads.Count): $($frameThreads -join ', ')."
    }
    $frameThread = [string]$frameThreads[0]
    for ($index = 1; $index -lt $sortedFrames.Count; ++$index) {
        $previous = $sortedFrames[$index - 1]
        $current = $sortedFrames[$index]
        if ($current.StartNs -lt $previous.EndNs) {
            throw "Overlapping '$FrameZoneName' intervals on thread '$frameThread': starts $($previous.StartNs) and $($current.StartNs)."
        }
    }

    $markerCounts = New-Object int[] $sortedFrames.Count
    $markersByFrame = New-Object object[] $sortedFrames.Count
    $sortedActiveZones = @($activeZones | Sort-Object StartNs)
    $frameIndex = 0
    foreach ($zone in $sortedActiveZones) {
        if (-not [string]::Equals(
            $zone.Thread,
            $frameThread,
            [System.StringComparison]::Ordinal)) {
            throw "Selection marker '$SelectionZoneName' was recorded on thread '$($zone.Thread)', expected '$frameThread'."
        }

        while ($frameIndex -lt $sortedFrames.Count -and
            $sortedFrames[$frameIndex].EndNs -le $zone.StartNs) {
            ++$frameIndex
        }
        if ($frameIndex -ge $sortedFrames.Count) {
            throw "Selection marker '$SelectionZoneName' at $($zone.StartNs) ns is outside every '$FrameZoneName' interval."
        }

        $frame = $sortedFrames[$frameIndex]
        if ($zone.EndNs -le $frame.StartNs) {
            throw "Selection marker '$SelectionZoneName' at $($zone.StartNs) ns falls between '$FrameZoneName' intervals."
        }
        if ($zone.StartNs -lt $frame.StartNs -or $zone.EndNs -gt $frame.EndNs) {
            throw "Selection marker '$SelectionZoneName' [$($zone.StartNs), $($zone.EndNs)) partially overlaps '$FrameZoneName' [$($frame.StartNs), $($frame.EndNs))."
        }

        ++$markerCounts[$frameIndex]
        if ($markerCounts[$frameIndex] -gt 1) {
            throw "Frame at $($frame.StartNs) ns contains more than one exact '$SelectionZoneName' marker."
        }
        $markersByFrame[$frameIndex] = $zone
    }

    $selected = New-Object 'System.Collections.Generic.List[object]'
    $selectedMarkers = New-Object 'System.Collections.Generic.List[object]'
    $runLengths = New-Object 'System.Collections.Generic.List[int]'
    $currentRunLength = 0
    for ($index = 0; $index -lt $sortedFrames.Count; ++$index) {
        if ($markerCounts[$index] -eq 1) {
            $selected.Add($sortedFrames[$index])
            $selectedMarkers.Add($markersByFrame[$index])
            ++$currentRunLength
        }
        elseif ($currentRunLength -gt 0) {
            $runLengths.Add($currentRunLength)
            $currentRunLength = 0
        }
    }
    if ($currentRunLength -gt 0) {
        $runLengths.Add($currentRunLength)
    }

    if ($selected.Count -ne $script:expectedActiveFrames) {
        throw "Expected exactly $($script:expectedActiveFrames) one-marker active frames, but selected $($selected.Count)."
    }
    if ($runLengths.Count -ne $script:captureBatches) {
        throw "Expected $($script:captureBatches) active-frame runs, but found $($runLengths.Count): $($runLengths.ToArray() -join ', ')."
    }
    for ($index = 0; $index -lt $runLengths.Count; ++$index) {
        if ($runLengths[$index] -ne $script:framesPerBatch) {
            throw "Active-frame run $($index + 1) contained $($runLengths[$index]) frames; expected $($script:framesPerBatch)."
        }
    }

    return [pscustomobject]@{
        Frames = $selected.ToArray()
        Markers = $selectedMarkers.ToArray()
        TotalFrameCount = $sortedFrames.Count
        SelectionMarkerCount = $activeZones.Count
        SelectionZoneName = $SelectionZoneName
        Thread = $frameThread
        RunLengths = $runLengths.ToArray()
    }
}

function Get-ZoneDurationsWithinFrames {
    param(
        [Parameter(Mandatory = $true)][object[]]$Frames,
        [Parameter(Mandatory = $true)][object[]]$Intervals
    )

    $intervalsByThread = @{}
    foreach ($interval in $Intervals) {
        if (-not $intervalsByThread.ContainsKey($interval.Thread)) {
            $intervalsByThread[$interval.Thread] = New-Object 'System.Collections.Generic.List[object]'
        }
        $intervalsByThread[$interval.Thread].Add($interval)
    }
    foreach ($thread in @($intervalsByThread.Keys)) {
        $intervalsByThread[$thread] = @($intervalsByThread[$thread] | Sort-Object StartNs)
    }

    $indices = @{}
    $durations = New-Object 'System.Collections.Generic.List[double]'
    $eventCounts = New-Object 'System.Collections.Generic.List[int]'
    $matchedFrameCount = 0
    $matchedEventCount = 0
    foreach ($frame in @($Frames | Sort-Object StartNs)) {
        [double]$durationNs = 0.0
        $frameEventCount = 0
        if ($intervalsByThread.ContainsKey($frame.Thread)) {
            $threadIntervals = @($intervalsByThread[$frame.Thread])
            $index = if ($indices.ContainsKey($frame.Thread)) { [int]$indices[$frame.Thread] } else { 0 }
            while ($index -lt $threadIntervals.Count -and $threadIntervals[$index].EndNs -le $frame.StartNs) {
                ++$index
            }
            $indices[$frame.Thread] = $index

            $scanIndex = $index
            while ($scanIndex -lt $threadIntervals.Count -and
                $threadIntervals[$scanIndex].StartNs -lt $frame.EndNs) {
                $interval = $threadIntervals[$scanIndex]
                if ($interval.EndNs -gt $frame.StartNs) {
                    if ($interval.StartNs -lt $frame.StartNs -or $interval.EndNs -gt $frame.EndNs) {
                        throw "Zone '$($interval.Name)' [$($interval.StartNs), $($interval.EndNs)) partially overlaps active frame [$($frame.StartNs), $($frame.EndNs))."
                    }
                    $durationNs += [double]$interval.DurationNs
                    ++$frameEventCount
                }
                ++$scanIndex
            }
        }
        if ($frameEventCount -gt 0) {
            ++$matchedFrameCount
            $matchedEventCount += $frameEventCount
        }
        $durations.Add($durationNs)
        $eventCounts.Add($frameEventCount)
    }

    return [pscustomobject]@{
        DurationsNs = $durations.ToArray()
        EventCounts = $eventCounts.ToArray()
        MatchedFrameCount = $matchedFrameCount
        MatchedEventCount = $matchedEventCount
    }
}

function Get-ExactlyOneZoneIntervalPerFrame {
    param(
        [Parameter(Mandatory = $true)][object[]]$Frames,
        [Parameter(Mandatory = $true)][object[]]$Intervals,
        [Parameter(Mandatory = $true)][string]$ExactZoneName,
        [switch]$AllowOutsideSelectedFrames
    )

    if ($Frames.Count -eq 0) {
        throw "Cannot validate '$ExactZoneName' without selected active frames."
    }
    if ($Intervals.Count -eq 0) {
        throw "Cannot validate '$ExactZoneName' without exact intervals."
    }

    $sortedFrames = @($Frames | Sort-Object StartNs)
    $frameThreads = @($sortedFrames | ForEach-Object { $_.Thread } | Sort-Object -Unique)
    if ($frameThreads.Count -ne 1) {
        throw "Strict zone validation requires active frames on exactly one thread."
    }
    $frameThread = [string]$frameThreads[0]
    $sortedIntervals = @($Intervals | Sort-Object StartNs)
    $intervalsByFrame = New-Object object[] $sortedFrames.Count
    $outsideIntervalCount = 0
    $frameIndex = 0

    foreach ($interval in $sortedIntervals) {
        if (-not [string]::Equals(
            [string]$interval.Thread,
            $frameThread,
            [System.StringComparison]::Ordinal)) {
            throw "Zone '$ExactZoneName' was recorded on thread '$($interval.Thread)', expected '$frameThread'."
        }

        while ($frameIndex -lt $sortedFrames.Count -and
            $sortedFrames[$frameIndex].EndNs -le $interval.StartNs) {
            ++$frameIndex
        }
        if ($frameIndex -ge $sortedFrames.Count) {
            ++$outsideIntervalCount
            continue
        }

        $frame = $sortedFrames[$frameIndex]
        if ($interval.EndNs -le $frame.StartNs) {
            ++$outsideIntervalCount
            continue
        }
        if ($interval.StartNs -lt $frame.StartNs -or $interval.EndNs -gt $frame.EndNs) {
            throw "Zone '$ExactZoneName' [$($interval.StartNs), $($interval.EndNs)) partially overlaps active frame [$($frame.StartNs), $($frame.EndNs))."
        }
        if ($null -ne $intervalsByFrame[$frameIndex]) {
            throw "Active frame $($frameIndex + 1) contains more than one exact '$ExactZoneName' interval."
        }
        $intervalsByFrame[$frameIndex] = $interval
    }

    for ($index = 0; $index -lt $intervalsByFrame.Count; ++$index) {
        if ($null -eq $intervalsByFrame[$index]) {
            throw "Active frame $($index + 1) is missing exact zone '$ExactZoneName'."
        }
    }
    if (-not $AllowOutsideSelectedFrames -and $outsideIntervalCount -ne 0) {
        throw "Zone '$ExactZoneName' has $outsideIntervalCount intervals outside the selected active frames; expected zero."
    }

    return [pscustomobject]@{
        Intervals = $intervalsByFrame
        ActiveIntervalCount = $intervalsByFrame.Count
        TotalIntervalCount = $sortedIntervals.Count
        OutsideIntervalCount = $outsideIntervalCount
        Thread = $frameThread
    }
}

function Get-LinearPercentile {
    param(
        [Parameter(Mandatory = $true)][double[]]$SortedValues,
        [Parameter(Mandatory = $true)][ValidateRange(0.0, 1.0)][double]$Percentile
    )

    if ($SortedValues.Count -eq 0) {
        throw "Cannot calculate a percentile from an empty sample."
    }
    $position = ($SortedValues.Count - 1) * $Percentile
    $lowerIndex = [int][Math]::Floor($position)
    $upperIndex = [int][Math]::Ceiling($position)
    if ($lowerIndex -eq $upperIndex) {
        return $SortedValues[$lowerIndex]
    }
    $fraction = $position - $lowerIndex
    return $SortedValues[$lowerIndex] +
        (($SortedValues[$upperIndex] - $SortedValues[$lowerIndex]) * $fraction)
}

function Get-DurationStatistics {
    param([Parameter(Mandatory = $true)][double[]]$DurationsNs)

    if ($DurationsNs.Count -eq 0) {
        throw "Cannot calculate duration statistics from an empty sample."
    }
    [double[]]$sortedNs = @($DurationsNs | Sort-Object)
    [double]$sumNs = 0.0
    foreach ($value in $sortedNs) {
        $sumNs += $value
    }
    $statisticsNs = [ordered]@{
        mean = [Math]::Round($sumNs / $sortedNs.Count, 3)
        median = [Math]::Round((Get-LinearPercentile -SortedValues $sortedNs -Percentile 0.50), 3)
        p95 = [Math]::Round((Get-LinearPercentile -SortedValues $sortedNs -Percentile 0.95), 3)
        p99 = [Math]::Round((Get-LinearPercentile -SortedValues $sortedNs -Percentile 0.99), 3)
        max = [Math]::Round($sortedNs[$sortedNs.Count - 1], 3)
    }
    $statisticsMs = [ordered]@{}
    foreach ($name in @("mean", "median", "p95", "p99", "max")) {
        $statisticsMs[$name] = [Math]::Round(([double]$statisticsNs[$name]) / 1000000.0, 6)
    }
    return [ordered]@{
        nanoseconds = $statisticsNs
        milliseconds = $statisticsMs
    }
}

function Get-FrameStatistics {
    param([Parameter(Mandatory = $true)][object[]]$Frames)

    [double[]]$durationsNs = @($Frames | ForEach-Object { [double]$_.DurationNs })
    return Get-DurationStatistics -DurationsNs $durationsNs
}

function Read-ActiveSessionOptional {
    if (-not (Test-Path -LiteralPath $script:activeSessionPath -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -Raw -LiteralPath $script:activeSessionPath | ConvertFrom-Json
    }
    catch {
        throw "The recorded Agent session is not valid JSON: $($script:activeSessionPath)"
    }
}

function Get-SessionIdentity {
    param($Owner)

    if ($null -eq $Owner) {
        return "<none>"
    }
    return "{0}:{1}:{2}" -f ([int]$Owner.pid), ([long]$Owner.processStartUtcTicks), ([string]$Owner.runDirectory)
}

function Test-RecordedProcessAlive {
    param($Owner)

    if ($null -eq $Owner) {
        return $false
    }
    $process = Get-Process -Id ([int]$Owner.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $false
    }
    try {
        return $process.StartTime.ToUniversalTime().Ticks -eq [long]$Owner.processStartUtcTicks
    }
    catch {
        return $false
    }
}

function Get-RecordedProcess {
    param($Owner)

    if (-not (Test-RecordedProcessAlive -Owner $Owner)) {
        return $null
    }
    try {
        return Get-Process -Id ([int]$Owner.pid) -ErrorAction Stop
    }
    catch {
        return $null
    }
}

function Assert-OwnedReadySession {
    param(
        [Parameter(Mandatory = $true)]$Owner,
        [Parameter(Mandatory = $true)][string]$PreviousIdentity,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifact,
        [Parameter(Mandatory = $true)][string]$ExpectedOwnerTag
    )

    if ([string]$Owner.state -ne "ready") {
        throw "The Agent session did not record state=ready."
    }
    if (-not [bool]$Owner.tracyEnabled) {
        throw "The Agent session did not record tracyEnabled=true."
    }
    if ($null -eq $Owner.PSObject.Properties["ownerTag"] -or
        -not [string]::Equals(
            [string]$Owner.ownerTag,
            $ExpectedOwnerTag,
            [System.StringComparison]::Ordinal)) {
        throw "The Agent session ownerTag does not match this steady profile invocation."
    }
    if ([string]::Equals((Get-SessionIdentity -Owner $Owner), $PreviousIdentity,
        [System.StringComparison]::Ordinal)) {
        throw "The Agent start did not publish a new session owner."
    }
    if (-not [string]::Equals(
        [System.IO.Path]::GetFullPath([string]$Owner.artifact),
        [System.IO.Path]::GetFullPath($ExpectedArtifact),
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The Agent session artifact is not the expected Tracy HGame artifact."
    }
    if (-not (Test-RecordedProcessAlive -Owner $Owner)) {
        throw "The Agent session PID/start-time identity is not alive."
    }
}

function Get-InvocationSessionCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$PreviousIdentity,
        [Parameter(Mandatory = $true)][DateTime]$InvocationUtc,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifact,
        [Parameter(Mandatory = $true)][string]$ExpectedOwnerTag
    )

    $candidate = Read-ActiveSessionOptional
    if ($null -eq $candidate -or
        [string]::Equals((Get-SessionIdentity -Owner $candidate), $PreviousIdentity,
            [System.StringComparison]::Ordinal) -or
        -not [bool]$candidate.tracyEnabled -or
        -not (Test-RecordedProcessAlive -Owner $candidate)) {
        return $null
    }
    if ($null -eq $candidate.PSObject.Properties["ownerTag"] -or
        -not [string]::Equals(
            [string]$candidate.ownerTag,
            $ExpectedOwnerTag,
            [System.StringComparison]::Ordinal)) {
        return $null
    }
    if (-not [string]::Equals(
        [System.IO.Path]::GetFullPath([string]$candidate.artifact),
        [System.IO.Path]::GetFullPath($ExpectedArtifact),
        [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }
    if ([long]$candidate.processStartUtcTicks -lt $InvocationUtc.AddSeconds(-2).Ticks) {
        return $null
    }

    # The unguessable ownerTag is written with the first active.json state, so
    # a partial start can be adopted before the READY line is emitted.
    return $candidate
}

function Test-CurrentInvocationSession {
    param(
        [Parameter(Mandatory = $true)]$Owner,
        [Parameter(Mandatory = $true)][string]$ExpectedOwnerTag
    )

    $currentOwner = Read-ActiveSessionOptional
    return $null -ne $currentOwner -and
        [string]::Equals(
            (Get-SessionIdentity -Owner $currentOwner),
            (Get-SessionIdentity -Owner $Owner),
            [System.StringComparison]::Ordinal) -and
        $null -ne $currentOwner.PSObject.Properties["ownerTag"] -and
        [string]::Equals(
            [string]$currentOwner.ownerTag,
            $ExpectedOwnerTag,
            [System.StringComparison]::Ordinal)
}

$bAnalyzeExisting = -not [string]::IsNullOrWhiteSpace($AnalyzeTracePath)
$tracyCapturePath = Join-Path $script:repositoryRoot "Engine\Tools\Tracy\tracy-capture.exe"
$tracyExporterPath = Join-Path $script:repositoryRoot "Engine\Tools\Tracy\tracy-csvexport.exe"
$resolvedAnalyzeTracePath = $null
$sourceTraceProvenance = $null

$analysisZoneNames = @(
    $FrameZoneName,
    $GameplayZoneName,
    $PhysicsZoneName,
    $PythonZoneName,
    $AnimationZoneName,
    $RenderZoneName,
    $PresentZoneName)
$uniqueAnalysisZoneNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($analysisZoneName in $analysisZoneNames) {
    if (-not $uniqueAnalysisZoneNames.Add($analysisZoneName)) {
        throw "Frame, gameplay, physics, Python, animation, render, and present zone names must be pairwise distinct."
    }
}

if ($bAnalyzeExisting) {
    if (-not (Test-Path -LiteralPath $AnalyzeTracePath -PathType Leaf)) {
        throw "-AnalyzeTracePath was not found: $AnalyzeTracePath"
    }
    $resolvedAnalyzeTracePath = (Resolve-Path -LiteralPath $AnalyzeTracePath).Path
    $sourceTraceProvenance = Get-SourceTraceProvenance -TracePath $resolvedAnalyzeTracePath
}
else {
    if ([double]::IsNaN($FixedDeltaSeconds) -or
        [double]::IsInfinity($FixedDeltaSeconds) -or
        $FixedDeltaSeconds -le 0.0 -or $FixedDeltaSeconds -gt 1.0) {
        throw "-FixedDeltaSeconds must be in the range (0, 1]."
    }
    if ($Resolution -notmatch "^(?<width>[1-9][0-9]{0,3})[xX](?<height>[1-9][0-9]{0,3})$") {
        throw "-Resolution must use WIDTHxHEIGHT."
    }
    if ([int]$matches.width -gt 8192 -or [int]$matches.height -gt 8192) {
        throw "-Resolution dimensions must be in the range 1..8192."
    }
    if ([double]::IsNaN($StartClickX) -or [double]::IsInfinity($StartClickX) -or
        [double]::IsNaN($StartClickY) -or [double]::IsInfinity($StartClickY)) {
        throw "Start click coordinates must be finite."
    }
    foreach ($requiredPath in @(
        $script:hectlPath,
        $script:hectlScriptPath,
        $script:sessionScriptPath,
        $tracyCapturePath)) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Capture dependency was not found: $requiredPath"
        }
    }
}
if (-not (Test-Path -LiteralPath $tracyExporterPath -PathType Leaf)) {
    throw "Required Tracy $($script:tracyVersion) exporter was not found: $tracyExporterPath"
}

$scriptFingerprint = Get-FileFingerprint -Path $PSCommandPath
$survivorGameScriptFingerprint = Get-FileFingerprint -Path (
    Join-Path $script:repositoryRoot "Project\Scripts\SurvivorGame.py")
$survivorGameScriptFingerprint["role"] = if ($bAnalyzeExisting) {
    "current-analysis-workspace; not asserted as the source trace input"
}
else {
    "captured loose runtime input"
}
$gitProvenance = Get-GitProvenance -RepositoryRoot $script:repositoryRoot
$toolMetadata = [ordered]@{
    csvexport = Get-FileFingerprint -Path $tracyExporterPath
    agentScripts = [ordered]@{
        process = Get-OptionalFileProvenance -Path (Join-Path $PSScriptRoot "Process.ps1")
        session = Get-OptionalFileProvenance -Path $script:sessionScriptPath
        hectl = Get-OptionalFileProvenance -Path $script:hectlScriptPath
        build = Get-OptionalFileProvenance -Path $script:buildScriptPath
    }
}
if (-not $bAnalyzeExisting) {
    $toolMetadata["capture"] = Get-FileFingerprint -Path $tracyCapturePath
}

New-Item -ItemType Directory -Force -Path $script:profilesRoot | Out-Null
$profileLockPath = Join-Path $script:profilesRoot ".tracy-profile.lock"
$profileLock = $null
try {
    $profileLock = [System.IO.File]::Open(
        $profileLockPath,
        [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
}
catch {
    throw "Another Tracy profile operation is already in progress."
}

$runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), [guid]::NewGuid().ToString("N").Substring(0, 12)
$outputDirectory = Join-Path $script:profilesRoot $runId
if (Test-Path -LiteralPath $outputDirectory) {
    $profileLock.Dispose()
    throw "Refusing to reuse an existing profile output directory: $outputDirectory"
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project (Join-Path $script:repositoryRoot 'Project') -Workflow 'Profile-Tracy' -Paths @($outputDirectory) -Locks @($profileLockPath) -Dependencies @($resolvedAnalyzeTracePath) -Outer
$storageState = 'unknown'
try {


$script:hectlLogPath = Join-Path $outputDirectory "hectl.log"
$captureTracePath = Join-Path $outputDirectory "capture.tracy"
if ($bAnalyzeExisting) {
    $tracePath = $resolvedAnalyzeTracePath
}
else {
    $tracePath = $captureTracePath
}
$captureStdoutPath = Join-Path $outputDirectory "capture.stdout.log"
$captureStderrPath = Join-Path $outputDirectory "capture.stderr.log"
$zoneDefinitions = [ordered]@{
    applicationFrame = [ordered]@{
        exactName = $FrameZoneName
        exportFilter = $FrameZoneName
        diagnosticSubstring = $FrameZoneName
        fileStem = "application-frame"
    }
    gameplayFrame = [ordered]@{
        exactName = $GameplayZoneName
        exportFilter = $GameplayZoneName
        diagnosticSubstring = $GameplayZoneName
        fileStem = "gameplay-frame"
        allowMissingInAnalyzeExisting = $true
    }
    survivorUpdate = [ordered]@{
        exactName = $script:survivorUpdateZoneName
        exportFilter = $script:survivorUpdateZoneName
        diagnosticSubstring = $script:survivorUpdateZoneName
        fileStem = "survivor-update"
        allowMissingInAnalyzeExisting = $true
    }
    survivorBookkeeping = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorBookkeeping
        exportFilter = $script:survivorPhaseZoneNames.survivorBookkeeping
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorBookkeeping
        fileStem = "survivor-phase-bookkeeping"
        allowMissingInAnalyzeExisting = $true
    }
    survivorPendingPickupSpawns = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorPendingPickupSpawns
        exportFilter = $script:survivorPhaseZoneNames.survivorPendingPickupSpawns
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorPendingPickupSpawns
        fileStem = "survivor-phase-pending-pickup-spawns"
        allowMissingInAnalyzeExisting = $true
    }
    survivorWaveSpawn = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorWaveSpawn
        exportFilter = $script:survivorPhaseZoneNames.survivorWaveSpawn
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorWaveSpawn
        fileStem = "survivor-phase-wave-spawn"
        allowMissingInAnalyzeExisting = $true
    }
    survivorMonsters = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorMonsters
        exportFilter = $script:survivorPhaseZoneNames.survivorMonsters
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorMonsters
        fileStem = "survivor-phase-monsters"
        allowMissingInAnalyzeExisting = $true
    }
    survivorPickups = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorPickups
        exportFilter = $script:survivorPhaseZoneNames.survivorPickups
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorPickups
        fileStem = "survivor-phase-pickups"
        allowMissingInAnalyzeExisting = $true
    }
    survivorWeapons = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorWeapons
        exportFilter = $script:survivorPhaseZoneNames.survivorWeapons
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorWeapons
        fileStem = "survivor-phase-weapons"
        allowMissingInAnalyzeExisting = $true
    }
    survivorHud = [ordered]@{
        exactName = $script:survivorPhaseZoneNames.survivorHud
        exportFilter = $script:survivorPhaseZoneNames.survivorHud
        diagnosticSubstring = $script:survivorPhaseZoneNames.survivorHud
        fileStem = "survivor-phase-hud"
        allowMissingInAnalyzeExisting = $true
    }
    layerPreUpdates = [ordered]@{
        exactName = "Layer Pre-Updates"
        exportFilter = "Layer Pre-Updates"
        diagnosticSubstring = "Layer Pre-Updates"
        fileStem = "layer-pre-updates"
    }
    layerUpdates = [ordered]@{
        exactName = "Layer Updates"
        exportFilter = "Layer Updates"
        diagnosticSubstring = "Layer Updates"
        fileStem = "layer-updates"
    }
    imguiFrame = [ordered]@{
        exactName = "ImGui Frame"
        exportFilter = "ImGui Frame"
        diagnosticSubstring = "ImGui Frame"
        fileStem = "imgui-frame"
    }
    windowUpdate = [ordered]@{
        exactName = "Window Update"
        exportFilter = "Window Update"
        diagnosticSubstring = "Window Update"
        fileStem = "window-update"
    }
    gameLayerUpdate = [ordered]@{
        exactName = "GameLayer::OnUpdate"
        exportFilter = "GameLayer::OnUpdate"
        diagnosticSubstring = "GameLayer::OnUpdate"
        fileStem = "game-layer-update"
        allowMissingInAnalyzeExisting = $true
    }
    sceneUpdateRuntime = [ordered]@{
        exactName = "GameLayer::SceneUpdateRuntime"
        exportFilter = "GameLayer::SceneUpdateRuntime"
        diagnosticSubstring = "GameLayer::SceneUpdateRuntime"
        fileStem = "scene-update-runtime"
        allowMissingInAnalyzeExisting = $true
    }
    rmlUiBeginFrame = [ordered]@{
        exactName = "RmlUi::BeginFrame"
        exportFilter = "RmlUi::BeginFrame"
        diagnosticSubstring = "RmlUi::BeginFrame"
        fileStem = "rmlui-begin-frame"
        allowMissingInAnalyzeExisting = $true
    }
    rmlUiUpdate = [ordered]@{
        exactName = "RmlUi::Update"
        exportFilter = "RmlUi::Update"
        diagnosticSubstring = "RmlUi::Update"
        fileStem = "rmlui-update"
        allowMissingInAnalyzeExisting = $true
    }
    rmlUiRender = [ordered]@{
        exactName = "RmlUi::Render"
        exportFilter = "RmlUi::Render"
        diagnosticSubstring = "RmlUi::Render"
        fileStem = "rmlui-render"
        allowMissingInAnalyzeExisting = $true
    }
    pythonScriptSystem = [ordered]@{
        exactName = $PythonZoneName
        exportFilter = $PythonZoneName
        diagnosticSubstring = $PythonZoneName
        fileStem = "python-script-system"
    }
    physicsSystem = [ordered]@{
        exactName = $PhysicsZoneName
        exportFilter = $PhysicsZoneName
        diagnosticSubstring = $PhysicsZoneName
        fileStem = "physics-system"
    }
    animationSystem = [ordered]@{
        exactName = $AnimationZoneName
        exportFilter = $AnimationZoneName
        diagnosticSubstring = $AnimationZoneName
        fileStem = "animation-system"
    }
    renderSystem = [ordered]@{
        exactName = $RenderZoneName
        exportFilter = $RenderZoneName
        diagnosticSubstring = $RenderZoneName
        fileStem = "render-system"
    }
    present = [ordered]@{
        exactName = $PresentZoneName
        exportFilter = $PresentZoneName
        diagnosticSubstring = $PresentZoneName
        fileStem = "present"
        # Present is represented by the raw GPU export on NVRHI backends and
        # does not necessarily have a CPU zone in an Agent capture.
        allowMissingAlways = $true
    }
}
$uniqueAnalysisExactNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($zoneKey in $zoneDefinitions.Keys) {
    $exactName = [string]$zoneDefinitions[$zoneKey].exactName
    if (-not $uniqueAnalysisExactNames.Add($exactName)) {
        throw "Duplicate analysis exact zone name '$exactName' at '$zoneKey'."
    }
}
$zoneCsvPaths = [ordered]@{}
$zoneStderrPaths = [ordered]@{}
foreach ($zoneKey in $zoneDefinitions.Keys) {
    $fileStem = [string]$zoneDefinitions[$zoneKey].fileStem
    $zoneCsvPaths[$zoneKey] = Join-Path $outputDirectory "$fileStem-events.csv"
    $zoneStderrPaths[$zoneKey] = Join-Path $outputDirectory "$fileStem-export.stderr.log"
}
$gpuCsvPath = Join-Path $outputDirectory "gpu-events.csv"
$gpuStderrPath = Join-Path $outputDirectory "gpu-export.stderr.log"
$activeFramesCsvPath = Join-Path $outputDirectory "active-frames.csv"
$metricsPath = Join-Path $outputDirectory "metrics.json"
$metadataPath = Join-Path $outputDirectory "metadata.json"

$startedUtc = [DateTime]::UtcNow
$ownerTag = if ($bAnalyzeExisting) { "" } else { [guid]::NewGuid().ToString("N") }
$script:ownerTag = $ownerTag
$expectedArtifact = Join-Path $script:repositoryRoot (
    "out\agent\game-tracy-{0}\bin\StarbloomSurvivor.exe" -f $Configuration.ToLowerInvariant())
$metadata = [ordered]@{
    schemaVersion = 4
    analysisRevision = $script:analysisRevision
    status = "running"
    tracyVersion = $script:tracyVersion
    startedUtc = $startedUtc.ToString("o")
    repositoryRoot = $script:repositoryRoot
    outputDirectory = $outputDirectory
    mode = if ($bAnalyzeExisting) { "analyze-existing" } else { "capture-and-analyze" }
    script = $scriptFingerprint
    powerShell = [ordered]@{
        edition = [string]$PSVersionTable.PSEdition
        version = $PSVersionTable.PSVersion.ToString()
    }
    git = $gitProvenance
    analysis = [ordered]@{
        percentileMethod = $script:percentileMethod
        captureBatches = $script:captureBatches
        framesPerBatch = $script:framesPerBatch
        expectedActiveFrames = $script:expectedActiveFrames
        frameZoneName = $FrameZoneName
        selectionPolicy = [ordered]@{
            captureRequiredZoneName = $GameplayZoneName
            legacyAnalyzeFallbackZoneName = $PhysicsZoneName
            legacyFallbackAllowedOnlyInAnalyzeExisting = $true
        }
        metricZones = [ordered]@{
            layerPreUpdates = "Layer Pre-Updates"
            layerUpdates = "Layer Updates"
            imguiFrame = "ImGui Frame"
            windowUpdate = "Window Update"
            gameLayerUpdate = "GameLayer::OnUpdate"
            sceneUpdateRuntime = "GameLayer::SceneUpdateRuntime"
            rmlUiBeginFrame = "RmlUi::BeginFrame"
            rmlUiUpdate = "RmlUi::Update"
            rmlUiRender = "RmlUi::Render"
            gameplayFrame = $GameplayZoneName
            survivorUpdate = $script:survivorUpdateZoneName
            survivorBookkeeping = $script:survivorPhaseZoneNames.survivorBookkeeping
            survivorPendingPickupSpawns = $script:survivorPhaseZoneNames.survivorPendingPickupSpawns
            survivorWaveSpawn = $script:survivorPhaseZoneNames.survivorWaveSpawn
            survivorMonsters = $script:survivorPhaseZoneNames.survivorMonsters
            survivorPickups = $script:survivorPhaseZoneNames.survivorPickups
            survivorWeapons = $script:survivorPhaseZoneNames.survivorWeapons
            survivorHud = $script:survivorPhaseZoneNames.survivorHud
            pythonScriptSystem = $PythonZoneName
            physicsSystem = $PhysicsZoneName
            animationSystem = $AnimationZoneName
            renderSystem = $RenderZoneName
            present = $PresentZoneName
        }
        survivorPythonPhasePolicy = [ordered]@{
            requiredForCurrentCapture = $true
            expectedActiveFrames = $script:expectedActiveFrames
            expectedIntervalsPerActiveFrame = 1
            parentZoneName = $script:survivorUpdateZoneName
            orderedChildZoneNames = @(
                $script:survivorPhaseChildKeys |
                    ForEach-Object { [string]$script:survivorPhaseZoneNames[$_] })
            selectionMarkerZoneName = $GameplayZoneName
            hierarchy = "children and final selection marker inside SurvivorGame::Update; SurvivorGame::Update inside PythonScriptSystem; ordered children do not overlap; per-frame child duration sum does not exceed parent"
            analyzeExistingCompatibility = "All eight Survivor zones may be absent together; partial availability is rejected."
        }
        containment = "exact zone names; one CPU thread; non-overlapping frame intervals; exactly one active marker and one Survivor phase interval fully contained by each selected frame; phase hierarchy/order enforced; partial overlaps rejected"
        gpuInterpretation = "Raw, unfiltered Tracy GPU export only; no GPU frame-time or CPU/GPU bottleneck conclusion."
    }
    tools = $toolMetadata
    profiledScript = $survivorGameScriptFingerprint
}
if ($bAnalyzeExisting) {
    $metadata["sourceTrace"] = $sourceTraceProvenance.trace
    $metadata["sourceMetadata"] = $sourceTraceProvenance.sidecar
}
else {
    $metadata["ownerTag"] = $ownerTag
    $metadata["captureConfiguration"] = [ordered]@{
        configuration = $Configuration
        resolution = $Resolution.ToLowerInvariant()
        fixedDeltaSeconds = $FixedDeltaSeconds
        timeouts = [ordered]@{
            buildSeconds = $BuildTimeoutSeconds
            agentReadyOrCallSeconds = $AgentTimeoutSeconds
            processBufferSeconds = $script:processTimeoutBufferSeconds
            agentStartProcessBudgetSeconds = $BuildTimeoutSeconds + $AgentTimeoutSeconds + `
                $script:processTimeoutBufferSeconds
            agentCallProcessBudgetSeconds = $AgentTimeoutSeconds + $script:processTimeoutBufferSeconds
            captureConnectSeconds = $CaptureConnectTimeoutSeconds
            captureShutdownSeconds = $CaptureShutdownTimeoutSeconds
            exportSeconds = $ExportTimeoutSeconds
        }
        interpretation = [ordered]@{
            fixedDelta = "Fixed delta is reproducible simulation input only. Wall-clock CPU metrics come from Tracy Application::Frame zones; the Frame Time plot is not used."
            gameplaySelection = "Capture acceptance requires exactly $($script:captureBatches)x$($script:framesPerBatch) '$GameplayZoneName' markers; '$PhysicsZoneName' is measured independently and cannot satisfy the current capture gate."
            upgradePolicy = "tracy-only deterministic first-choice auto-select"
            survivalPolicy = "tracy-only restore max health before each active gameplay frame"
        }
        recipe = [ordered]@{
            startClick = [ordered]@{ x = $StartClickX; y = $StartClickY; button = "left" }
            warmupBatches = $script:warmupBatches
            captureBatches = $script:captureBatches
            framesPerBatch = $script:framesPerBatch
            upgradePolicy = "tracy-only deterministic first-choice auto-select"
            survivalPolicy = "tracy-only restore max health before each active gameplay frame"
        }
    }
}

$preexistingOwner = $null
$preexistingIdentity = "<none>"
$agentStartAttempted = $false
$agentStartInvocationUtc = [DateTime]::MinValue
$sessionStarted = $false
$sessionOwner = $null
$captureProcess = $null
$runError = $null
$cleanupErrors = New-Object 'System.Collections.Generic.List[string]'
$batchTimings = New-Object 'System.Collections.Generic.List[object]'
$agentStartProcessTimeoutSeconds = $BuildTimeoutSeconds + $AgentTimeoutSeconds + `
    $script:processTimeoutBufferSeconds
$bSucceeded = $false
$previousSurvivorAutoplay = [Environment]::GetEnvironmentVariable(
    "HE_SURVIVOR_AUTOPLAY", "Process")
$bOverrodeSurvivorAutoplay = $false

try {
    $script:toolJob = New-AgentProcessJob
    if (-not $bAnalyzeExisting) {
        [Environment]::SetEnvironmentVariable(
            "HE_SURVIVOR_AUTOPLAY", "1", "Process")
        $bOverrodeSurvivorAutoplay = $true
        $preexistingOwner = Read-ActiveSessionOptional
        $preexistingIdentity = Get-SessionIdentity -Owner $preexistingOwner
        if (Test-RecordedProcessAlive -Owner $preexistingOwner) {
            throw "An Agent session is already running (pid=$($preexistingOwner.pid)); steady profiling will not disturb it."
        }

        $existingCapture = Get-AgentArtifactProcess -ArtifactPath $tracyCapturePath `
            -ProcessName "tracy-capture"
        if ($null -ne $existingCapture) {
            throw "The repository Tracy capture tool is already running: pid=$($existingCapture.Id)."
        }

        $fixedDeltaText = $FixedDeltaSeconds.ToString(
            "R", [System.Globalization.CultureInfo]::InvariantCulture)

        $agentStartAttempted = $true
        $agentStartInvocationUtc = [DateTime]::UtcNow
        $agentStartArguments = @(
            "agent", "start",
            "-Configuration", $Configuration,
            "-Parallel", ([string]$Parallel),
            "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
            "-FixedDeltaSeconds", $fixedDeltaText,
            "-Resolution", $Resolution,
            "-EnableTracy",
            "-AgentOwnerTag", $ownerTag
        )
        if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
            $agentStartArguments += @("-StartupScene", $StartupScene)
        }
        $null = Invoke-HeCtl -Label "Tracy Agent start" -Arguments $agentStartArguments `
            -ProcessTimeoutSeconds $agentStartProcessTimeoutSeconds
        $startedOwner = Read-ActiveSessionOptional
        if ($null -eq $startedOwner) {
            throw "Agent start succeeded without publishing active.json."
        }
        Assert-OwnedReadySession -Owner $startedOwner -PreviousIdentity $preexistingIdentity `
            -ExpectedArtifact $expectedArtifact -ExpectedOwnerTag $ownerTag
        $sessionOwner = $startedOwner
        $sessionStarted = $true

        if (-not (Test-Path -LiteralPath ([string]$sessionOwner.artifact) -PathType Leaf)) {
            throw "The Agent session HGame artifact was not found: $($sessionOwner.artifact)"
        }
        $gameArtifactFingerprint = Get-FileFingerprint -Path ([string]$sessionOwner.artifact)
        $metadata["session"] = [ordered]@{
            pid = [int]$sessionOwner.pid
            processStartUtcTicks = [long]$sessionOwner.processStartUtcTicks
            runDirectory = [string]$sessionOwner.runDirectory
            artifact = $gameArtifactFingerprint.path
            artifactLength = $gameArtifactFingerprint.length
            artifactSha256 = $gameArtifactFingerprint.sha256
            tracyEnabled = [bool]$sessionOwner.tracyEnabled
            ownerTag = [string]$sessionOwner.ownerTag
        }

        if ([string]::IsNullOrWhiteSpace($StartupScene)) {
            $clickParamsJson = [ordered]@{
                x = $StartClickX
                y = $StartClickY
                button = "left"
            } | ConvertTo-Json -Compress
            $null = Invoke-AgentSession -Label "Start-screen click" `
                -Action "Call" -Method "input.click" -ParamsJson $clickParamsJson
        }

        for ($batch = 1; $batch -le $script:warmupBatches; ++$batch) {
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            $stepParamsJson = "{`"frames`":$($script:framesPerBatch)}"
            $null = Invoke-AgentSession -Label "Warm-up batch $batch/$($script:warmupBatches)" `
                -Action "Call" -Method "runtime.step" -ParamsJson $stepParamsJson
            $stopwatch.Stop()
            $batchTimings.Add([ordered]@{
                phase = "warmup"
                batch = $batch
                frames = $script:framesPerBatch
                wallSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 6)
            })
        }

        Assert-NewFile -Path $tracePath
        $captureProcess = Start-OwnedProcess -FilePath $tracyCapturePath `
            -Arguments @("-o", $tracePath, "-a", $script:tracyAddress, "-p", ([string]$script:tracyPort)) `
            -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath
        $metadata.session["connection"] = Wait-TracyCaptureConnection `
            -Process $captureProcess -GamePid ([int]$sessionOwner.pid) `
            -GameStartUtcTicks ([long]$sessionOwner.processStartUtcTicks) `
            -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath

        for ($batch = 1; $batch -le $script:captureBatches; ++$batch) {
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            $stepParamsJson = "{`"frames`":$($script:framesPerBatch)}"
            $null = Invoke-AgentSession -Label "Profile batch $batch/$($script:captureBatches)" `
                -Action "Call" -Method "runtime.step" -ParamsJson $stepParamsJson
            $stopwatch.Stop()
            $batchTimings.Add([ordered]@{
                phase = "capture"
                batch = $batch
                frames = $script:framesPerBatch
                wallSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 6)
            })
        }

        # A normal client disconnect asks tracy-capture to finish and save the trace.
        if (-not (Test-CurrentInvocationSession -Owner $sessionOwner -ExpectedOwnerTag $ownerTag)) {
            throw "The active Agent owner changed before the normal Tracy stop; refusing to stop another session."
        }
        $null = Invoke-AgentSession -Label "Tracy Agent stop" -Action "Stop"
        $sessionStarted = $false
        Wait-OwnedProcess -Process $captureProcess `
            -TimeoutSeconds $CaptureShutdownTimeoutSeconds `
            -Description "tracy-capture" `
            -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath

        if (Select-String -LiteralPath $captureStdoutPath `
            -SimpleMatch "Instrumentation failure:" -Quiet) {
            throw "tracy-capture reported an instrumentation failure; inspect $captureStdoutPath"
        }

        if (-not (Test-Path -LiteralPath $tracePath -PathType Leaf) -or
            (Get-Item -LiteralPath $tracePath).Length -eq 0) {
            throw "tracy-capture exited successfully but did not produce a trace: $tracePath"
        }
        $saveMarkerObserved = [bool](Select-String -LiteralPath $captureStdoutPath `
            -SimpleMatch "Saving trace... done!" -Quiet)
        if (-not $saveMarkerObserved) {
            throw "tracy-capture exited without the required 'Saving trace... done!' marker."
        }
        $metadata["captureCompletion"] = [ordered]@{
            exitCode = $captureProcess.ExitCode
            saveMarkerObserved = $saveMarkerObserved
        }
        $metadata["capturedTrace"] = Get-FileFingerprint -Path $tracePath
    }

    foreach ($zoneKey in $zoneDefinitions.Keys) {
        $zone = $zoneDefinitions[$zoneKey]
        Invoke-TracyExport -ExporterPath $tracyExporterPath `
            -Arguments @("-u", "-f", ([string]$zone.exportFilter), $tracePath) `
            -CsvPath ([string]$zoneCsvPaths[$zoneKey]) `
            -StderrPath ([string]$zoneStderrPaths[$zoneKey]) `
            -Label "Tracy CPU zone export: $zoneKey"
    }
    Invoke-TracyExport -ExporterPath $tracyExporterPath `
        -Arguments @("-g", $tracePath) `
        -CsvPath $gpuCsvPath -StderrPath $gpuStderrPath `
        -Label "Tracy GPU CSV export"

    if ($bAnalyzeExisting) {
        $sourceTraceAfterExport = Get-FileFingerprint -Path $tracePath
        if ($sourceTraceAfterExport.length -ne $sourceTraceProvenance.trace.length -or
            -not [string]::Equals(
                $sourceTraceAfterExport.sha256,
                $sourceTraceProvenance.trace.sha256,
                [System.StringComparison]::Ordinal)) {
            throw "The source trace changed while it was being analyzed: $tracePath"
        }
        $metadata.sourceTrace["verifiedUnchangedAfterExport"] = $true
    }

    $gpuManifest = Get-GpuCsvManifest -CsvPath $gpuCsvPath
    $selectionSource = "gameplay-marker"
    $selectionZoneName = $GameplayZoneName
    $selectionCsvPath = [string]$zoneCsvPaths.gameplayFrame
    $bLegacySelectionFallback = $false
    if ($bAnalyzeExisting) {
        $gameplaySelectionIntervals = @(Read-ExactZoneIntervals `
            -CsvPath $selectionCsvPath `
            -ExactZoneName $GameplayZoneName `
            -DiagnosticSubstring $GameplayZoneName `
            -AllowEmpty)
        if ($gameplaySelectionIntervals.Count -eq 0) {
            $selectionSource = "legacy-physics-fallback"
            $selectionZoneName = $PhysicsZoneName
            $selectionCsvPath = [string]$zoneCsvPaths.physicsSystem
            $bLegacySelectionFallback = $true
        }
    }

    $activeSelection = Select-ActiveFrames `
        -FrameCsvPath ([string]$zoneCsvPaths.applicationFrame) `
        -SelectionCsvPath $selectionCsvPath `
        -SelectionZoneName $selectionZoneName
    $activeFrames = @($activeSelection.Frames)
    if ($activeFrames.Count -ne $script:expectedActiveFrames) {
        throw "Expected exactly $($script:expectedActiveFrames) active frames from $($script:captureBatches)x$($script:framesPerBatch), but selected $($activeFrames.Count)."
    }
    $selectionResult = [ordered]@{
        source = $selectionSource
        zoneName = $selectionZoneName
        legacyFallback = $bLegacySelectionFallback
        currentCaptureGateSatisfied = -not $bLegacySelectionFallback
    }
    if ($bLegacySelectionFallback) {
        $selectionResult["warning"] = "Legacy analyze-only fallback to the physics marker; this result cannot satisfy the current capture gate."
    }
    $metadata.analysis["selectionResult"] = $selectionResult

    $activeFrameRows = for ($index = 0; $index -lt $activeFrames.Count; ++$index) {
        $frame = $activeFrames[$index]
        [pscustomobject]@{
            index = $index + 1
            thread = $frame.Thread
            ns_since_start = $frame.StartNs
            exec_time_ns = $frame.DurationNs
            exec_time_ms = [Math]::Round(([double]$frame.DurationNs) / 1000000.0, 6)
        }
    }
    Assert-NewFile -Path $activeFramesCsvPath
    $activeFrameRows | Export-Csv -LiteralPath $activeFramesCsvPath `
        -NoTypeInformation -Encoding UTF8

    $frameStatistics = Get-FrameStatistics -Frames $activeFrames
    [double[]]$frameDurationsNs = @($activeFrames | ForEach-Object { [double]$_.DurationNs })
    $zoneDurationArrays = [ordered]@{
        applicationFrame = $frameDurationsNs
    }
    $zoneCoverage = [ordered]@{}
    $zoneIntervals = [ordered]@{}
    $unavailableZoneKeys = New-Object 'System.Collections.Generic.List[string]'
    $zoneMetrics = [ordered]@{
        applicationFrame = [ordered]@{
            status = "available"
            zoneName = $FrameZoneName
            durationInterpretation = "Inclusive main-thread wall-clock interval."
            sampleCount = $activeFrames.Count
            matchedFrameCount = $activeFrames.Count
            matchedEventCount = $activeFrames.Count
            statistics = $frameStatistics
        }
    }
    foreach ($zoneKey in @(
        "layerPreUpdates",
        "layerUpdates",
        "imguiFrame",
        "windowUpdate",
        "gameLayerUpdate",
        "sceneUpdateRuntime",
        "rmlUiBeginFrame",
        "rmlUiUpdate",
        "rmlUiRender",
        "gameplayFrame",
        "survivorUpdate",
        "survivorBookkeeping",
        "survivorPendingPickupSpawns",
        "survivorWaveSpawn",
        "survivorMonsters",
        "survivorPickups",
        "survivorWeapons",
        "survivorHud",
        "pythonScriptSystem",
        "physicsSystem",
        "animationSystem",
        "renderSystem",
        "present")) {
        $zone = $zoneDefinitions[$zoneKey]
        $bAllowMissing = [bool]$zone.allowMissingAlways -or (
            $bAnalyzeExisting -and [bool]$zone.allowMissingInAnalyzeExisting
        )
        $intervals = @(Read-ExactZoneIntervals `
            -CsvPath ([string]$zoneCsvPaths[$zoneKey]) `
            -ExactZoneName ([string]$zone.exactName) `
            -DiagnosticSubstring ([string]$zone.diagnosticSubstring) `
            -AllowEmpty:$bAllowMissing)
        if ($intervals.Count -eq 0) {
            $unavailableZoneKeys.Add($zoneKey)
            $zoneMetrics[$zoneKey] = [ordered]@{
                status = "unavailable"
                zoneName = [string]$zone.exactName
                reason = "Optional zone is absent from this trace."
            }
            continue
        }

        $zoneIntervals[$zoneKey] = $intervals
        $coverage = Get-ZoneDurationsWithinFrames -Frames $activeFrames -Intervals $intervals
        if ($coverage.MatchedFrameCount -ne $activeFrames.Count) {
            throw "Zone '$($zone.exactName)' matched $($coverage.MatchedFrameCount)/$($activeFrames.Count) active frames."
        }
        $zoneCoverage[$zoneKey] = $coverage
        $zoneDurationArrays[$zoneKey] = $coverage.DurationsNs
        $zoneMetrics[$zoneKey] = [ordered]@{
            status = "available"
            zoneName = [string]$zone.exactName
            durationInterpretation = "Sum of inclusive, fully-contained main-thread intervals per selected frame; zone metrics may be nested and are not additive."
            sampleCount = $coverage.DurationsNs.Count
            matchedFrameCount = $coverage.MatchedFrameCount
            matchedEventCount = $coverage.MatchedEventCount
            statistics = Get-DurationStatistics -DurationsNs $coverage.DurationsNs
        }
    }

    $availableSurvivorProfileZoneKeys = @(
        $script:survivorProfileZoneKeys |
            Where-Object { $null -ne $zoneIntervals[$_] })
    if ($availableSurvivorProfileZoneKeys.Count -ne 0 -and
        $availableSurvivorProfileZoneKeys.Count -ne $script:survivorProfileZoneKeys.Count) {
        $missingPhaseKeys = @(
            $script:survivorProfileZoneKeys |
                Where-Object { $null -eq $zoneIntervals[$_] })
        throw "Survivor Python phase zones are only partially available. Missing: $($missingPhaseKeys -join ', ')."
    }
    if (-not $bAnalyzeExisting -and $availableSurvivorProfileZoneKeys.Count -eq 0) {
        throw "A new capture must contain all Survivor Python phase zones."
    }
    if ($bLegacySelectionFallback -and
        $availableSurvivorProfileZoneKeys.Count -eq $script:survivorProfileZoneKeys.Count) {
        throw "Survivor Python phase zones require the exact '$GameplayZoneName' selection marker; legacy physics fallback cannot validate their hierarchy."
    }

    $survivorPhaseAnalysis = [ordered]@{
        status = "unavailable"
        phaseGateSatisfied = $false
        expectedActiveFrameCount = $script:expectedActiveFrames
        expectedIntervalsPerActiveFrame = 1
        parentZoneName = $script:survivorUpdateZoneName
        orderedChildZoneNames = @(
            $script:survivorPhaseChildKeys |
                ForEach-Object { [string]$script:survivorPhaseZoneNames[$_] })
        selectionMarkerZoneName = $GameplayZoneName
        reason = "All eight Survivor Python phase zones are absent from this older source trace."
    }
    if ($availableSurvivorProfileZoneKeys.Count -eq $script:survivorProfileZoneKeys.Count) {
        $strictZoneMappings = [ordered]@{}
        $activeIntervalCounts = [ordered]@{}
        $totalIntervalCounts = [ordered]@{}
        $outsideIntervalCounts = [ordered]@{}
        $phaseStatistics = [ordered]@{}
        foreach ($zoneKey in $script:survivorProfileZoneKeys) {
            $zone = $zoneDefinitions[$zoneKey]
            $mapping = Get-ExactlyOneZoneIntervalPerFrame `
                -Frames $activeFrames `
                -Intervals @($zoneIntervals[$zoneKey]) `
                -ExactZoneName ([string]$zone.exactName) `
                -AllowOutsideSelectedFrames:($zoneKey -eq "survivorUpdate")
            $strictZoneMappings[$zoneKey] = $mapping
            $activeIntervalCounts[$zoneKey] = $mapping.ActiveIntervalCount
            $totalIntervalCounts[$zoneKey] = $mapping.TotalIntervalCount
            $outsideIntervalCounts[$zoneKey] = $mapping.OutsideIntervalCount
            $phaseStatistics[$zoneKey] = [ordered]@{
                zoneName = [string]$zone.exactName
                statistics = $zoneMetrics[$zoneKey].statistics
            }
            $zoneMetrics[$zoneKey]["strictPerActiveFrame"] = $true
            $zoneMetrics[$zoneKey]["expectedEventCountPerActiveFrame"] = 1
            $zoneMetrics[$zoneKey]["activeIntervalCount"] = $mapping.ActiveIntervalCount
            $zoneMetrics[$zoneKey]["totalIntervalCount"] = $mapping.TotalIntervalCount
            $zoneMetrics[$zoneKey]["outsideSelectedFrameCount"] = $mapping.OutsideIntervalCount
        }

        $pythonSystemMapping = Get-ExactlyOneZoneIntervalPerFrame `
            -Frames $activeFrames `
            -Intervals @($zoneIntervals.pythonScriptSystem) `
            -ExactZoneName $PythonZoneName `
            -AllowOutsideSelectedFrames
        $activeMarkers = @($activeSelection.Markers)
        if ($activeMarkers.Count -ne $activeFrames.Count) {
            throw "Selection marker mapping contains $($activeMarkers.Count) intervals; expected $($activeFrames.Count)."
        }

        [double[]]$survivorChildDurationSumsNs = New-Object double[] $activeFrames.Count
        [double[]]$survivorUnattributedDurationsNs = New-Object double[] $activeFrames.Count
        for ($frameIndex = 0; $frameIndex -lt $activeFrames.Count; ++$frameIndex) {
            $pythonInterval = $pythonSystemMapping.Intervals[$frameIndex]
            $parentInterval = $strictZoneMappings.survivorUpdate.Intervals[$frameIndex]
            if ($parentInterval.StartNs -lt $pythonInterval.StartNs -or
                $parentInterval.EndNs -gt $pythonInterval.EndNs) {
                throw "Active frame $($frameIndex + 1) has '$($script:survivorUpdateZoneName)' outside '$PythonZoneName'."
            }

            [long]$childDurationSumNs = 0
            $previousChild = $null
            foreach ($childKey in $script:survivorPhaseChildKeys) {
                $childInterval = $strictZoneMappings[$childKey].Intervals[$frameIndex]
                if ($childInterval.StartNs -lt $parentInterval.StartNs -or
                    $childInterval.EndNs -gt $parentInterval.EndNs) {
                    throw "Active frame $($frameIndex + 1) has '$($zoneDefinitions[$childKey].exactName)' outside '$($script:survivorUpdateZoneName)'."
                }
                if ($null -ne $previousChild -and
                    $childInterval.StartNs -lt $previousChild.EndNs) {
                    throw "Active frame $($frameIndex + 1) has out-of-order or overlapping Survivor child phases '$($previousChild.Name)' and '$($childInterval.Name)'."
                }
                $childDurationSumNs += [long]$childInterval.DurationNs
                $previousChild = $childInterval
            }

            $markerInterval = $activeMarkers[$frameIndex]
            if ($markerInterval.StartNs -lt $parentInterval.StartNs -or
                $markerInterval.EndNs -gt $parentInterval.EndNs) {
                throw "Active frame $($frameIndex + 1) has selection marker '$GameplayZoneName' outside '$($script:survivorUpdateZoneName)'."
            }
            if ($null -eq $previousChild -or
                $markerInterval.StartNs -lt $previousChild.EndNs) {
                throw "Active frame $($frameIndex + 1) has selection marker '$GameplayZoneName' before the final Survivor child phase completed."
            }
            if ($childDurationSumNs -gt [long]$parentInterval.DurationNs) {
                throw "Active frame $($frameIndex + 1) has Survivor child duration sum greater than '$($script:survivorUpdateZoneName)'."
            }

            $survivorChildDurationSumsNs[$frameIndex] = [double]$childDurationSumNs
            $survivorUnattributedDurationsNs[$frameIndex] =
                [double]([long]$parentInterval.DurationNs - $childDurationSumNs)
        }

        $survivorPhaseAnalysis = [ordered]@{
            status = "available"
            phaseGateSatisfied = $true
            expectedActiveFrameCount = $script:expectedActiveFrames
            expectedIntervalsPerActiveFrame = 1
            parentZoneName = $script:survivorUpdateZoneName
            orderedChildZoneNames = @(
                $script:survivorPhaseChildKeys |
                    ForEach-Object { [string]$script:survivorPhaseZoneNames[$_] })
            selectionMarkerZoneName = $GameplayZoneName
            exactNameMatch = $true
            sameThreadAsActiveFrames = $true
            exactlyOneIntervalPerActiveFrame = $true
            fullyContainedByActiveFrame = $true
            parentContainedByPythonScriptSystem = $true
            childrenContainedByParent = $true
            childrenOrderedAndNonOverlapping = $true
            markerAfterFinalChildAndContainedByParent = $true
            childDurationSumWithinParent = $true
            activeIntervalCounts = $activeIntervalCounts
            totalIntervalCounts = $totalIntervalCounts
            outsideSelectedFrameCounts = $outsideIntervalCounts
            pythonScriptSystemActiveIntervalCount = $pythonSystemMapping.ActiveIntervalCount
            statistics = $phaseStatistics
            childDurationSumStatistics = Get-DurationStatistics `
                -DurationsNs $survivorChildDurationSumsNs
            unattributedParentDurationStatistics = Get-DurationStatistics `
                -DurationsNs $survivorUnattributedDurationsNs
        }
    }
    $metadata.analysis["survivorPythonPhaseResult"] = [ordered]@{
        status = $survivorPhaseAnalysis.status
        phaseGateSatisfied = $survivorPhaseAnalysis.phaseGateSatisfied
        expectedActiveFrameCount = $script:expectedActiveFrames
        availableZoneCount = $availableSurvivorProfileZoneKeys.Count
        expectedZoneCount = $script:survivorProfileZoneKeys.Count
    }

    $preUpdateCoverage = $zoneCoverage.layerPreUpdates
    if ($null -eq $preUpdateCoverage -or
        $preUpdateCoverage.MatchedEventCount -ne $activeFrames.Count) {
        throw "Layer Pre-Updates must provide exactly one source interval for every active frame."
    }

    [double[]]$frameMinusLayerPreUpdatesNs = New-Object double[] $activeFrames.Count
    for ($index = 0; $index -lt $activeFrames.Count; ++$index) {
        if ($preUpdateCoverage.EventCounts[$index] -ne 1) {
            throw "Active frame $($index + 1) matched $($preUpdateCoverage.EventCounts[$index]) 'Layer Pre-Updates' intervals; expected exactly one."
        }
        if ($zoneDurationArrays.layerPreUpdates[$index] -gt $frameDurationsNs[$index]) {
            throw "Active frame $($index + 1) has Layer Pre-Updates duration greater than Application::Frame duration."
        }
        $adjusted = $frameDurationsNs[$index] - $zoneDurationArrays.layerPreUpdates[$index]
        if ($adjusted -lt 0.0) {
            throw "Active frame $($index + 1) produced a negative Application::Frame minus Layer Pre-Updates duration."
        }
        $frameMinusLayerPreUpdatesNs[$index] = $adjusted
    }
    $derivedMetrics = [ordered]@{
        applicationFrameMinusLayerPreUpdates = [ordered]@{
            metricName = "Application::Frame minus Layer Pre-Updates"
            classification = "diagnostic-arithmetic"
            interpretation = "Per-frame arithmetic subtraction for this HE_DEV_AGENT_RUNTIME trace. It is not a production frame-time estimate because Layer Pre-Updates is a generic Layer interval and other Agent/Tracy effects remain."
            sourcePairCount = $activeFrames.Count
            sampleCount = $activeFrames.Count
            statistics = Get-DurationStatistics -DurationsNs $frameMinusLayerPreUpdatesNs
        }
    }

    $survivorGameScriptAfterAnalysis = Get-FileFingerprint -Path (
        [string]$survivorGameScriptFingerprint.path)
    if ($survivorGameScriptAfterAnalysis.length -ne $survivorGameScriptFingerprint.length -or
        -not [string]::Equals(
            $survivorGameScriptAfterAnalysis.sha256,
            $survivorGameScriptFingerprint.sha256,
            [System.StringComparison]::Ordinal)) {
        throw "Project/Scripts/SurvivorGame.py changed while the Tracy profile was running or being analyzed."
    }
    $survivorGameScriptFingerprint["verifiedUnchangedThroughAnalysis"] = $true

    $metrics = [ordered]@{
        schemaVersion = 4
        analysisRevision = $script:analysisRevision
        profiledScript = $survivorGameScriptFingerprint
        sampleCount = $activeFrames.Count
        percentileMethod = $script:percentileMethod
        selection = [ordered]@{
            frameZoneName = $FrameZoneName
            source = $selectionSource
            selectionZoneName = $selectionZoneName
            primaryGameplayZoneName = $GameplayZoneName
            legacyFallbackZoneName = $PhysicsZoneName
            legacyFallback = $bLegacySelectionFallback
            currentCaptureGateSatisfied = -not $bLegacySelectionFallback
            exactNameMatch = $true
            sameThread = $true
            nonOverlappingFrames = $true
            fullyContainedInterval = $true
            partialOverlapsRejected = $true
            exactlyOneActiveMarkerPerSelectedFrame = $true
            totalFrameCount = $activeSelection.TotalFrameCount
            selectionMarkerCount = $activeSelection.SelectionMarkerCount
            thread = $activeSelection.Thread
            expectedRunCount = $script:captureBatches
            expectedFramesPerRun = $script:framesPerBatch
            observedRunLengths = $activeSelection.RunLengths
        }
        statistics = $frameStatistics
        zones = $zoneMetrics
        pythonPhases = $survivorPhaseAnalysis
        derived = $derivedMetrics
        gpu = [ordered]@{
            classification = $gpuManifest.classification
            eventCount = $gpuManifest.rowCount
            scope = $gpuManifest.scope
            frameStatisticsAvailable = $false
            conclusion = $gpuManifest.conclusion
        }
    }
    Write-NewJson -Path $metricsPath -Value $metrics

    if (-not $bAnalyzeExisting) {
        $metadata["batchTimings"] = $batchTimings.ToArray()
    }
    if ($unavailableZoneKeys.Count -gt 0) {
        $metadata.analysis["unavailableLegacyZones"] = $unavailableZoneKeys.ToArray()
    }
    $artifactMetadata = [ordered]@{
        trace = [ordered]@{
            path = $tracePath
            role = if ($bAnalyzeExisting) { "external-source" } else { "captured-output" }
            ownedByOutputDirectory = -not $bAnalyzeExisting
        }
        cpuZoneEvents = $zoneCsvPaths
        gpuEvents = $gpuManifest
        activeFrames = $activeFramesCsvPath
        metrics = $metricsPath
    }
    if (Test-Path -LiteralPath $script:hectlLogPath -PathType Leaf) {
        $artifactMetadata["hectlLog"] = $script:hectlLogPath
    }
    if (-not $bAnalyzeExisting) {
        $artifactMetadata["captureStdout"] = $captureStdoutPath
        $artifactMetadata["captureStderr"] = $captureStderrPath
    }
    $metadata["artifacts"] = $artifactMetadata
    $metadata["status"] = "pass"
    $bSucceeded = $true
}
catch {
    $runError = $_
    $metadata["status"] = "fail"
    $metadata["error"] = $_.Exception.Message
    $metadata["errorPosition"] = $_.InvocationInfo.PositionMessage
    $metadata["scriptStackTrace"] = $_.ScriptStackTrace
}
finally {
    if ($bOverrodeSurvivorAutoplay) {
        [Environment]::SetEnvironmentVariable(
            "HE_SURVIVOR_AUTOPLAY", $previousSurvivorAutoplay, "Process")
    }
    if (-not $sessionStarted -and $null -eq $sessionOwner -and $agentStartAttempted) {
        try {
            $candidate = Get-InvocationSessionCandidate `
                -PreviousIdentity $preexistingIdentity `
                -InvocationUtc $agentStartInvocationUtc `
                -ExpectedArtifact $expectedArtifact `
                -ExpectedOwnerTag $ownerTag
            if ($null -ne $candidate) {
                $sessionOwner = $candidate
                $sessionStarted = $true
                $metadata["adoptedSessionAfterStartFailure"] = $true
            }
        }
        catch {
            $cleanupErrors.Add("Failed to inspect a partial Agent start: $($_.Exception.Message)")
        }
    }

    if ($sessionStarted) {
        $bSessionStillCurrent = $false
        try {
            $bSessionStillCurrent = Test-CurrentInvocationSession `
                -Owner $sessionOwner -ExpectedOwnerTag $ownerTag
        }
        catch {
            $cleanupErrors.Add("Failed to verify the current Agent owner before cleanup: $($_.Exception.Message)")
        }

        if ($bSessionStillCurrent) {
            try {
                $null = Invoke-AgentSession -Label "Tracy Agent cleanup stop" -Action "Stop"
                $sessionStarted = $false
            }
            catch {
                $cleanupErrors.Add("Agent cleanup stop failed: $($_.Exception.Message)")
            }
        }
        else {
            $cleanupErrors.Add("Skipped global Agent cleanup stop because active owner identity/ownerTag no longer matched this invocation.")
        }

        if ($sessionStarted -and $null -ne $sessionOwner -and
            [string]$sessionOwner.jobName -match '^Local\\HEngineAgentSession_[0-9a-f]{32}$') {
            try {
                [void](Stop-AgentProcessJobByName -Name ([string]$sessionOwner.jobName))
            }
            catch {
                $cleanupErrors.Add("Agent Job cleanup failed: $($_.Exception.Message)")
            }
        }
    }

    try {
        $recordedProcess = Get-RecordedProcess -Owner $sessionOwner
        if ($null -ne $recordedProcess) {
            Stop-AgentOwnedProcess -Process $recordedProcess `
                -Description "Tracy Agent game cleanup"
        }
        if (Test-RecordedProcessAlive -Owner $sessionOwner) {
            $cleanupErrors.Add("Agent game process remained alive after exact-process cleanup: pid=$($sessionOwner.pid)")
        }
    }
    catch {
        $cleanupErrors.Add("Agent process cleanup failed: $($_.Exception.Message)")
    }

    if ($null -ne $captureProcess) {
        try {
            $captureProcess.Refresh()
            if (-not $captureProcess.HasExited) {
                if (-not $captureProcess.WaitForExit($CaptureShutdownTimeoutSeconds * 1000)) {
                    Stop-AgentOwnedProcess -Process $captureProcess `
                        -Description "tracy-capture cleanup"
                }
            }
            $captureProcess.WaitForExit()
        }
        catch {
            $cleanupErrors.Add("tracy-capture cleanup failed: $($_.Exception.Message)")
        }
    }

    if ($null -ne $script:toolJob) {
        try {
            Close-AgentProcessJob -Job $script:toolJob
        }
        catch {
            $cleanupErrors.Add("Tracy tool Job cleanup failed: $($_.Exception.Message)")
        }
    }
    if ($null -ne $profileLock) {
        $profileLock.Dispose()
    }

    $metadata["completedUtc"] = [DateTime]::UtcNow.ToString("o")
    $metadata["elapsedSeconds"] = [Math]::Round(
        ([DateTime]::UtcNow - $startedUtc).TotalSeconds, 6)
    if ($cleanupErrors.Count -gt 0) {
        $metadata["status"] = "fail"
        $metadata["cleanupErrors"] = $cleanupErrors.ToArray()
    }
    try {
        Write-NewJson -Path $metadataPath -Value $metadata
    }
    catch {
        $cleanupErrors.Add("metadata.json write failed: $($_.Exception.Message)")
    }
}

if ($null -ne $runError -or $cleanupErrors.Count -gt 0 -or -not $bSucceeded) {
    if ($null -ne $runError) {
        Write-Error $runError.Exception.Message -ErrorAction Continue
    }
    foreach ($cleanupError in $cleanupErrors) {
        Write-Error $cleanupError -ErrorAction Continue
    }
    Write-Host "partial profile artifacts: $outputDirectory"
    $storageState = $(if ($cleanupErrors.Count -gt 0) { 'unknown' } else { 'failed' })
    exit 1
}

$metricsSummary = Get-Content -Raw -LiteralPath $metricsPath | ConvertFrom-Json
$milliseconds = $metricsSummary.statistics.milliseconds
$selectionLabel = if ([bool]$metricsSummary.selection.legacyFallback) {
    "; legacyFallback=true; currentCaptureGateSatisfied=false"
}
else {
    "; currentCaptureGateSatisfied=true"
}
$phaseLabel = "; phaseGateSatisfied={0}" -f (
    [bool]$metricsSummary.pythonPhases.phaseGateSatisfied).ToString().ToLowerInvariant()
Write-Host ("Tracy profile: PASS; frames={0}; mean={1:n3} ms; median={2:n3} ms; p95={3:n3} ms; p99={4:n3} ms; max={5:n3} ms" -f `
    $metricsSummary.sampleCount,
    [double]$milliseconds.mean,
    [double]$milliseconds.median,
    [double]$milliseconds.p95,
    [double]$milliseconds.p99,
    [double]$milliseconds.max) -NoNewline
Write-Host ($selectionLabel + $phaseLabel)
Write-Host "profile artifacts: $outputDirectory"
$storageState = 'success'
exit 0

}
finally {
    if ($profileLock) { $profileLock.Dispose() }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
