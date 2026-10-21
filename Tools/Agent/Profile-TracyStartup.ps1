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
    [int]$CaptureArmTimeoutSeconds = 10,

    [ValidateRange(100, 5000)]
    [int]$CaptureArmMilliseconds = 750,

    [ValidateRange(1, 120)]
    [int]$CaptureConnectTimeoutSeconds = 30,

    [ValidateRange(1, 300)]
    [int]$CaptureShutdownTimeoutSeconds = 60,

    [ValidateRange(1, 600)]
    [int]$ExportTimeoutSeconds = 300,

    [double]$FixedDeltaSeconds = 0.0166667,

    [string]$Resolution = "1280x720"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Process.ps1")

$script:repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$script:hectlScriptPath = Join-Path $PSScriptRoot "hectl.ps1"
$script:sessionScriptPath = Join-Path $PSScriptRoot "Session.ps1"
$script:buildScriptPath = Join-Path $PSScriptRoot "Build.ps1"
$script:processScriptPath = Join-Path $PSScriptRoot "Process.ps1"
$script:profilesRoot = Join-Path $script:repositoryRoot "out\agent\profiles"
$script:activeSessionPath = Join-Path $script:repositoryRoot "out\agent\sessions\active.json"
$script:hectlLogPath = $null
$script:hectlCallIndex = 0
$script:lastHeCtlStdoutPath = $null
$script:lastHeCtlStderrPath = $null
$script:hectlTimings = $null
$script:toolJob = $null
$script:tracyAddress = "127.0.0.1"
$script:tracyPort = 8086
$script:tracyVersion = "0.13.1"
$script:processTimeoutBufferSeconds = 30
$script:startupAnalysisRevision = "async-startup-window-v4"
$script:startupMetadataSchemaVersion = 4
$script:startupMetricsSchemaVersion = 5

function Assert-NewFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to overwrite an existing startup profile artifact: $Path"
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

    Write-NewUtf8Text -Path $Path -Contents ($Value | ConvertTo-Json -Depth 20)
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

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
        return -join @($sha256.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
    }
    finally {
        $sha256.Dispose()
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
    $script:lastHeCtlStdoutPath = $stdoutPath
    $script:lastHeCtlStderrPath = $stderrPath

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

    $startedUtc = [DateTime]::UtcNow
    $process = $null
    $exitCode = $null
    $failure = $null
    $terminationFailure = $null
    try {
        $process = Start-Process -FilePath "powershell.exe" -ArgumentList $nativeArguments `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath `
            -WindowStyle Hidden -PassThru
        $null = $process.Handle
        # Keep every hectl wrapper and its descendants in the profile Job. The
        # Session-owned HGame also retains its named Job for cross-call cleanup.
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

    $completedUtc = [DateTime]::UtcNow
    $stdout = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    $stderr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    $output = @($stdout) + @($stderr)
    $elapsed = $completedUtc - $startedUtc
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
        "[{0:o}] {1}" -f $startedUtc, $Label
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

    if ($null -ne $script:hectlTimings) {
        $timing = [ordered]@{
            operation = $Label
            startedUtc = $startedUtc.ToString("o")
            completedUtc = $completedUtc.ToString("o")
            elapsedSeconds = [Math]::Round($elapsed.TotalSeconds, 6)
            processBudgetSeconds = $ProcessTimeoutSeconds
            exitCode = $exitCode
            status = if ($null -eq $failure -and $null -eq $terminationFailure) { "pass" } else { "fail" }
            stdout = $stdoutPath
            stderr = $stderrPath
        }
        if ($null -ne $failure) {
            $timing["error"] = $failure.Exception.Message
        }
        if ($null -ne $terminationFailure) {
            $timing["terminationError"] = $terminationFailure
        }
        $script:hectlTimings.Add($timing)
    }

    if ($null -ne $failure) {
        throw $failure
    }
    if ($null -ne $terminationFailure) {
        throw "$Label cleanup failed after process completion: $terminationFailure"
    }
    Write-Host ("{0}: PASS ({1:n1}s)" -f $Label, $elapsed.TotalSeconds)
    return [pscustomobject]@{
        Stdout = $stdout
        Stderr = $stderr
        StdoutPath = $stdoutPath
        StderrPath = $stderrPath
        StartedUtc = $startedUtc
        CompletedUtc = $completedUtc
        ElapsedSeconds = $elapsed.TotalSeconds
    }
}

function ConvertFrom-HeCtlEnvelope {
    param(
        [Parameter(Mandatory = $true)]$CallResult,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $candidates = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in @($CallResult.Stdout)) {
        if ([string]::IsNullOrWhiteSpace([string]$line)) {
            continue
        }
        try {
            $candidate = [string]$line | ConvertFrom-Json
            if ($null -ne $candidate.PSObject.Properties["protocol"] -and
                $null -ne $candidate.PSObject.Properties["ok"] -and
                [int]$candidate.protocol -eq 1) {
                $candidates.Add($candidate)
            }
        }
        catch {
        }
    }
    if ($candidates.Count -ne 1) {
        throw "$Label returned $($candidates.Count) protocol=1 JSON envelopes; expected exactly one."
    }
    $envelope = $candidates[0]
    if (-not [bool]$envelope.ok) {
        throw "$Label returned ok=false."
    }
    return $envelope
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

function Wait-TracyCaptureArmed {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )

    $marker = "Connecting to $($script:tracyAddress):$($script:tracyPort)..."
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $markerObservedUtc = $null
    $markerDwell = $null
    while ($stopwatch.Elapsed.TotalSeconds -lt $CaptureArmTimeoutSeconds) {
        $Process.Refresh()
        if ($Process.HasExited) {
            $Process.WaitForExit()
            $stdout = Get-LogTail -Path $StdoutPath
            $stderr = Get-LogTail -Path $StderrPath
            throw "tracy-capture exited before Agent start (exit=$($Process.ExitCode)).`nstdout:`n$stdout`nstderr:`n$stderr"
        }

        if ($null -eq $markerObservedUtc -and
            (Test-Path -LiteralPath $StdoutPath -PathType Leaf)) {
            $bMarkerFound = $false
            try {
                $bMarkerFound = [bool](Select-String -LiteralPath $StdoutPath `
                    -SimpleMatch $marker -Quiet -ErrorAction Stop)
            }
            catch {
                $bMarkerFound = $false
            }
            if ($bMarkerFound) {
                $markerObservedUtc = [DateTime]::UtcNow
                # Stability dwell starts only after the official marker is
                # visible; process-start time is not a substitute.
                $markerDwell = [System.Diagnostics.Stopwatch]::StartNew()
            }
        }

        if ($null -ne $markerDwell -and
            $markerDwell.ElapsedMilliseconds -ge $CaptureArmMilliseconds) {
            $prematureConnections = @(Get-NetTCPConnection -OwningProcess $Process.Id `
                -State Established -ErrorAction SilentlyContinue | Where-Object {
                    [int]$_.RemotePort -eq $script:tracyPort
                })
            if ($prematureConnections.Count -gt 0) {
                throw "tracy-capture connected to port $($script:tracyPort) before this script started its Agent session."
            }
            Write-Host "tracy-capture: ARMED pid=$($Process.Id) markerObserved=True"
            return [ordered]@{
                armedUtc = [DateTime]::UtcNow.ToString("o")
                markerObservedUtc = $markerObservedUtc.ToString("o")
                markerDwellMilliseconds = $CaptureArmMilliseconds
                stdoutMarker = $marker
                stdoutMarkerObserved = $true
            }
        }
        Start-Sleep -Milliseconds 50
    }

    $stdout = Get-LogTail -Path $StdoutPath
    $stderr = Get-LogTail -Path $StderrPath
    $phase = if ($null -eq $markerObservedUtc) { "official marker" } else { "post-marker dwell" }
    throw "tracy-capture did not complete its $phase within $CaptureArmTimeoutSeconds seconds.`nstdout:`n$stdout`nstderr:`n$stderr"
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
        [Parameter(Mandatory = $true)][string]$ExactZoneName
    )

    $expectedHeader = "name,src_file,src_line,ns_since_start,exec_time_ns,thread,value"
    $actualHeader = Get-Content -LiteralPath $CsvPath -TotalCount 1
    if (-not [string]::Equals($actualHeader, $expectedHeader, [System.StringComparison]::Ordinal)) {
        throw "Unexpected Tracy $($script:tracyVersion) CPU CSV header. Expected '$expectedHeader', got '$actualHeader'."
    }

    $intervals = New-Object 'System.Collections.Generic.List[object]'
    $nearbyNames = @{}
    $exportedNames = @{}
    [long]$exportedRowCount = 0
    Import-Csv -LiteralPath $CsvPath | ForEach-Object {
        $name = [string]$_.name
        ++$exportedRowCount
        $exportedNames[$name] = $true
        if ([string]::Equals($name, $ExactZoneName, [System.StringComparison]::Ordinal)) {
            $intervals.Add((ConvertTo-ZoneInterval -Row $_ -CsvPath $CsvPath))
        }
        elseif ($name.IndexOf($ExactZoneName, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $ExactZoneName.IndexOf($name, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $nearbyNames[$name] = $true
        }
    }
    return [pscustomobject]@{
        Intervals = $intervals.ToArray()
        NearbyNames = @($nearbyNames.Keys | Sort-Object)
        ExportedNames = @($exportedNames.Keys | Sort-Object)
        ExportedRowCount = $exportedRowCount
    }
}

function Get-SectionMetrics {
    param(
        [Parameter(Mandatory = $true)][string]$ExactZoneName,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Intervals
    )

    [long]$totalNs = 0
    [long]$maxNs = 0
    [long]$firstStartNs = [long]::MaxValue
    [long]$lastEndNs = 0
    foreach ($interval in $Intervals) {
        [long]$durationNs = $interval.DurationNs
        if ($totalNs -gt [long]::MaxValue - $durationNs) {
            throw "Total duration overflow for startup zone '$ExactZoneName'."
        }
        $totalNs += $durationNs
        $maxNs = [Math]::Max($maxNs, $durationNs)
        $firstStartNs = [Math]::Min($firstStartNs, [long]$interval.StartNs)
        $lastEndNs = [Math]::Max($lastEndNs, [long]$interval.EndNs)
    }

    return [ordered]@{
        zoneName = $ExactZoneName
        count = $Intervals.Count
        totalNanoseconds = $totalNs
        totalMilliseconds = [Math]::Round(([double]$totalNs) / 1000000.0, 6)
        maxNanoseconds = $maxNs
        maxMilliseconds = [Math]::Round(([double]$maxNs) / 1000000.0, 6)
        firstStartNanoseconds = if ($Intervals.Count -gt 0) { $firstStartNs } else { $null }
        lastEndNanoseconds = if ($Intervals.Count -gt 0) { $lastEndNs } else { $null }
    }
}

function Get-NearestRankPercentileNanoseconds {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Intervals,
        [Parameter(Mandatory = $true)][ValidateRange(1, 100)][int]$Percentile
    )

    if ($Intervals.Count -eq 0) {
        return [long]0
    }

    [long[]]$durations = @($Intervals | ForEach-Object { [long]$_.DurationNs })
    [Array]::Sort($durations)
    [int]$rank = [int][Math]::Ceiling(
        ([double]$Percentile / 100.0) * [double]$durations.Count)
    return [long]$durations[$rank - 1]
}

function Get-AnchorCardinalityValidation {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Requirements
    )

    $failures = New-Object 'System.Collections.Generic.List[string]'
    $bMissingAsyncAnchor = $false
    foreach ($requirement in $Requirements) {
        if ($requirement.Count -ne 1) {
            $failures.Add(
                "expected exactly one $($requirement.Name), found $($requirement.Count)")
            if ($requirement.Count -eq 0 -and [bool]$requirement.bAsync) {
                $bMissingAsyncAnchor = $true
            }
        }
    }
    return [pscustomobject]@{
        Failures = $failures.ToArray()
        MissingAsyncAnchor = $bMissingAsyncAnchor
    }
}

function Select-StartupWindowIntervals {
    param(
        [Parameter(Mandatory = $true)]$StartupWindow,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Intervals
    )

    $contained = New-Object 'System.Collections.Generic.List[object]'
    $outside = New-Object 'System.Collections.Generic.List[object]'
    $partial = New-Object 'System.Collections.Generic.List[object]'
    foreach ($interval in $Intervals) {
        if ($interval.EndNs -le $StartupWindow.StartNs -or
            $interval.StartNs -ge $StartupWindow.EndNs) {
            $outside.Add($interval)
            continue
        }
        if ($interval.StartNs -lt $StartupWindow.StartNs -or
            $interval.EndNs -gt $StartupWindow.EndNs) {
            $partial.Add($interval)
            continue
        }
        $contained.Add($interval)
    }
    return [pscustomobject]@{
        Contained = $contained.ToArray()
        OutsideWindow = $outside.ToArray()
        PartialOverlap = $partial.ToArray()
    }
}

function Select-EnvironmentWarmupStageIntervals {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Intervals,
        [Parameter(Mandatory = $true)]$FirstPresent,
        [Parameter(Mandatory = $true)]$RuntimeStart
    )

    $build = New-Object 'System.Collections.Generic.List[object]'
    $invalid = New-Object 'System.Collections.Generic.List[object]'
    foreach ($interval in $Intervals) {
        if ($interval.StartNs -ge $FirstPresent.EndNs -and
            $interval.EndNs -le $RuntimeStart.StartNs) {
            $build.Add($interval)
        }
        else {
            $invalid.Add($interval)
        }
    }
    return [pscustomobject]@{
        Build = $build.ToArray()
        Invalid = $invalid.ToArray()
    }
}

function Select-CommonContainingIntervals {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ParentIntervals,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ChildIntervals,
        [bool]$RequireSameThread = $true
    )

    if ($ChildIntervals.Count -eq 0) {
        return @()
    }

    $matches = New-Object 'System.Collections.Generic.List[object]'
    foreach ($parent in $ParentIntervals) {
        $bContainsAll = $true
        foreach ($child in $ChildIntervals) {
            if (($RequireSameThread -and -not [string]::Equals(
                [string]$parent.Thread,
                [string]$child.Thread,
                [System.StringComparison]::Ordinal)) -or
                $child.StartNs -lt $parent.StartNs -or
                $child.EndNs -gt $parent.EndNs) {
                $bContainsAll = $false
                break
            }
        }
        if ($bContainsAll) {
            $matches.Add($parent)
        }
    }
    return $matches.ToArray()
}

function Get-IntervalContainmentViolations {
    param(
        [Parameter(Mandatory = $true)][string]$Relationship,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ParentIntervals,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ChildIntervals,
        [bool]$RequireSameThread = $true
    )

    $violations = New-Object 'System.Collections.Generic.List[object]'
    foreach ($child in $ChildIntervals) {
        $bContained = $false
        foreach ($parent in $ParentIntervals) {
            if ($RequireSameThread -and -not [string]::Equals(
                [string]$child.Thread,
                [string]$parent.Thread,
                [System.StringComparison]::Ordinal)) {
                continue
            }
            if ($child.StartNs -ge $parent.StartNs -and $child.EndNs -le $parent.EndNs) {
                $bContained = $true
                break
            }
        }
        if (-not $bContained) {
            $violations.Add([pscustomobject][ordered]@{
                relationship = $Relationship
                requireSameThread = $RequireSameThread
                child = [ordered]@{
                    classification = "uncontained-child"
                    name = [string]$child.Name
                    thread = [string]$child.Thread
                    startNanoseconds = [long]$child.StartNs
                    endNanoseconds = [long]$child.EndNs
                    durationNanoseconds = [long]$child.DurationNs
                }
            })
        }
    }
    return $violations.ToArray()
}

function Get-IntervalOrderViolation {
    param(
        [Parameter(Mandatory = $true)][string]$Relationship,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$BeforeIntervals,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$AfterIntervals
    )

    if ($BeforeIntervals.Count -eq 0 -or $AfterIntervals.Count -eq 0) {
        return @()
    }

    [long]$latestBeforeEndNs = 0
    foreach ($interval in $BeforeIntervals) {
        $latestBeforeEndNs = [Math]::Max($latestBeforeEndNs, [long]$interval.EndNs)
    }
    [long]$earliestAfterStartNs = [long]::MaxValue
    foreach ($interval in $AfterIntervals) {
        $earliestAfterStartNs = [Math]::Min($earliestAfterStartNs, [long]$interval.StartNs)
    }
    if ($latestBeforeEndNs -le $earliestAfterStartNs) {
        return @()
    }

    return ,([pscustomobject][ordered]@{
        relationship = $Relationship
        latestBeforeEndNanoseconds = $latestBeforeEndNs
        earliestAfterStartNanoseconds = $earliestAfterStartNs
    })
}

function ConvertTo-IntervalEvidence {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Intervals,
        [Parameter(Mandatory = $true)][string]$Classification
    )

    return @($Intervals | ForEach-Object {
        [ordered]@{
            classification = $Classification
            name = [string]$_.Name
            thread = [string]$_.Thread
            startNanoseconds = [long]$_.StartNs
            endNanoseconds = [long]$_.EndNs
            durationNanoseconds = [long]$_.DurationNs
        }
    })
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
    return Get-Process -Id ([int]$Owner.pid) -ErrorAction SilentlyContinue
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
        throw "The Agent session ownerTag does not match this startup profile invocation."
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
foreach ($agentScriptPath in @(
    $script:processScriptPath,
    $script:sessionScriptPath,
    $script:hectlScriptPath,
    $script:buildScriptPath)) {
    if (-not (Test-Path -LiteralPath $agentScriptPath -PathType Leaf)) {
        throw "Required Agent script was not found: $agentScriptPath"
    }
}
if ($null -eq (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue)) {
    throw "Get-NetTCPConnection is required to verify Tracy capture ownership."
}

$tracyCapturePath = Join-Path $script:repositoryRoot "Engine\Tools\Tracy\tracy-capture.exe"
$tracyExporterPath = Join-Path $script:repositoryRoot "Engine\Tools\Tracy\tracy-csvexport.exe"
foreach ($toolPath in @($tracyCapturePath, $tracyExporterPath)) {
    if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
        throw "Required Tracy $($script:tracyVersion) tool was not found: $toolPath"
    }
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

$runId = "startup-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), [guid]::NewGuid().ToString("N").Substring(0, 12)
$outputDirectory = Join-Path $script:profilesRoot $runId
if (Test-Path -LiteralPath $outputDirectory) {
    $profileLock.Dispose()
    throw "Refusing to reuse an existing startup profile output directory: $outputDirectory"
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project (Join-Path $script:repositoryRoot 'Project') -Workflow 'Profile-TracyStartup' -Paths @($outputDirectory) -Locks @($profileLockPath) -Outer
$storageState = 'unknown'
try {


$script:hectlLogPath = Join-Path $outputDirectory "hectl.log"
$tracePath = Join-Path $outputDirectory "startup.tracy"
$captureStdoutPath = Join-Path $outputDirectory "capture.stdout.log"
$captureStderrPath = Join-Path $outputDirectory "capture.stderr.log"
$worldPath = Join-Path $outputDirectory "world.json"
$logsPath = Join-Path $outputDirectory "logs.json"
$gitStatusPath = Join-Path $outputDirectory "git-status.txt"
$sectionEventsPath = Join-Path $outputDirectory "startup-section-events.csv"
$metricsPath = Join-Path $outputDirectory "startup-metrics.json"
$metadataPath = Join-Path $outputDirectory "metadata.json"

$zoneDefinitions = [ordered]@{
    gameLayerOnAttach = [ordered]@{ exactName = "GameLayer::OnAttach"; fileStem = "game-layer-on-attach" }
    gameLayerCreateFramebuffer = [ordered]@{ exactName = "GameLayer::CreateFramebuffer"; fileStem = "game-layer-create-framebuffer" }
    gameLayerLoadGameConfig = [ordered]@{ exactName = "GameLayer::LoadGameConfig"; fileStem = "game-layer-load-game-config" }
    sceneSerializerDeserialize = [ordered]@{ exactName = "SceneSerializer::Deserialize"; fileStem = "scene-serializer-deserialize" }
    meshLoaderLoadModel = [ordered]@{ exactName = "MeshLoader::LoadModel"; fileStem = "mesh-loader-load-model" }
    meshLoaderLoadModelHmesh = [ordered]@{ exactName = "MeshLoader::LoadModelHMESH"; fileStem = "mesh-loader-load-model-hmesh" }
    gameLayerRuntimeStart = [ordered]@{ exactName = "GameLayer::RuntimeStart"; fileStem = "game-layer-runtime-start" }
    pythonScriptSystemRuntimeStart = [ordered]@{ exactName = "PythonScriptSystem::RuntimeStart"; fileStem = "python-script-system-runtime-start" }
    scriptEngineRegisterModule = [ordered]@{ exactName = "ScriptEngine::RegisterModule"; fileStem = "script-engine-register-module" }
    pythonScriptSystemStartCallback = [ordered]@{ exactName = "PythonScriptSystem::StartCallback"; fileStem = "python-script-system-start-callback" }
    rmlUiManagerLoadDocument = [ordered]@{ exactName = "RmlUiManager::LoadDocument"; fileStem = "rmlui-manager-load-document" }
    gameLayerAgentRuntimeStart = [ordered]@{ exactName = "GameLayer::AgentRuntimeStart"; fileStem = "game-layer-agent-runtime-start" }
    environmentSystemWarmupStep = [ordered]@{ exactName = "EnvironmentSystem::WarmupStep"; fileStem = "environment-system-warmup-step" }
    environmentCommitAcquireCubeMapLibrary = [ordered]@{ exactName = "EnvironmentSystem::Commit::AcquireCubeMapLibrary"; fileStem = "environment-commit-acquire-cubemap-library" }
    environmentCommitCacheInstall = [ordered]@{ exactName = "EnvironmentSystem::Commit::CacheInstall"; fileStem = "environment-commit-cache-install" }
    environmentCommitLibrarySet = [ordered]@{ exactName = "EnvironmentSystem::Commit::LibrarySet"; fileStem = "environment-commit-library-set" }
    environmentCommitReleaseStaging = [ordered]@{ exactName = "EnvironmentSystem::Commit::ReleaseStaging"; fileStem = "environment-commit-release-staging" }
}
$anchorDefinitions = [ordered]@{
    runtimeStartupFirstPresent = [ordered]@{ exactName = "Application::RuntimeStartupFirstPresent"; fileStem = "runtime-startup-first-present" }
    runtimeStartupFirstGameplayPresent = [ordered]@{ exactName = "Application::RuntimeStartupFirstGameplayPresent"; fileStem = "runtime-startup-first-gameplay-present" }
    gameLayerStartupReady = [ordered]@{ exactName = "GameLayer::StartupReady"; fileStem = "game-layer-startup-ready" }
}
$requiredStartupAnchorNames = @(
    [string]$zoneDefinitions.gameLayerOnAttach.exactName,
    [string]$anchorDefinitions.runtimeStartupFirstPresent.exactName,
    [string]$anchorDefinitions.runtimeStartupFirstGameplayPresent.exactName,
    [string]$anchorDefinitions.gameLayerStartupReady.exactName
)
$warmupStepKey = "environmentSystemWarmupStep"
$warmupBuildExpectedCount = 68
$warmupInsideExpectedCount = $warmupBuildExpectedCount
$warmupPercentile = 95
$commitSectionKeys = @(
    "environmentCommitAcquireCubeMapLibrary",
    "environmentCommitCacheInstall",
    "environmentCommitLibrarySet",
    "environmentCommitReleaseStaging"
)
$commitExpectedChildCount = 1
$commitExpectedParentOrdinal = $warmupBuildExpectedCount
$containmentRelationships = @(
    [pscustomobject]@{ Name = "CreateFramebuffer inside OnAttach"; Parent = "gameLayerOnAttach"; Child = "gameLayerCreateFramebuffer"; RequireSameThread = $true },
    [pscustomobject]@{ Name = "LoadModelHMESH inside LoadModel"; Parent = "meshLoaderLoadModel"; Child = "meshLoaderLoadModelHmesh"; RequireSameThread = $true },
    [pscustomobject]@{ Name = "Python runtime start inside GameLayer runtime start"; Parent = "gameLayerRuntimeStart"; Child = "pythonScriptSystemRuntimeStart"; RequireSameThread = $true },
    [pscustomobject]@{ Name = "RegisterModule inside Python runtime start"; Parent = "pythonScriptSystemRuntimeStart"; Child = "scriptEngineRegisterModule"; RequireSameThread = $true },
    [pscustomobject]@{ Name = "StartCallback inside Python runtime start"; Parent = "pythonScriptSystemRuntimeStart"; Child = "pythonScriptSystemStartCallback"; RequireSameThread = $true },
    [pscustomobject]@{ Name = "RmlUi LoadDocument inside StartCallback"; Parent = "pythonScriptSystemStartCallback"; Child = "rmlUiManagerLoadDocument"; RequireSameThread = $true }
)
$orderRelationships = @(
    [pscustomobject]@{ Name = "First present before game config"; Before = "runtimeStartupFirstPresent"; After = "gameLayerLoadGameConfig" },
    [pscustomobject]@{ Name = "Game config before scene deserialize"; Before = "gameLayerLoadGameConfig"; After = "sceneSerializerDeserialize" },
    [pscustomobject]@{ Name = "Scene deserialize before mesh decode"; Before = "sceneSerializerDeserialize"; After = "meshLoaderLoadModel" },
    [pscustomobject]@{ Name = "Mesh decode before runtime start"; Before = "meshLoaderLoadModel"; After = "gameLayerRuntimeStart" },
    [pscustomobject]@{ Name = "Runtime start before first gameplay present"; Before = "gameLayerRuntimeStart"; After = "runtimeStartupFirstGameplayPresent" },
    [pscustomobject]@{ Name = "RmlUi load before first gameplay present"; Before = "rmlUiManagerLoadDocument"; After = "runtimeStartupFirstGameplayPresent" },
    [pscustomobject]@{ Name = "First gameplay present before Agent start"; Before = "runtimeStartupFirstGameplayPresent"; After = "gameLayerAgentRuntimeStart" },
    [pscustomobject]@{ Name = "Agent start before StartupReady"; Before = "gameLayerAgentRuntimeStart"; After = "gameLayerStartupReady" }
)
$commitOrderRelationships = @(
    [pscustomobject]@{ Name = "Commit AcquireCubeMapLibrary before CacheInstall"; Before = "environmentCommitAcquireCubeMapLibrary"; After = "environmentCommitCacheInstall" },
    [pscustomobject]@{ Name = "Commit CacheInstall before LibrarySet"; Before = "environmentCommitCacheInstall"; After = "environmentCommitLibrarySet" },
    [pscustomobject]@{ Name = "Commit LibrarySet before ReleaseStaging"; Before = "environmentCommitLibrarySet"; After = "environmentCommitReleaseStaging" }
)
$analysisContractLines = New-Object 'System.Collections.Generic.List[string]'
$analysisContractLines.Add("revision=$($script:startupAnalysisRevision)")
$analysisContractLines.Add("metadataSchemaVersion=$($script:startupMetadataSchemaVersion)")
$analysisContractLines.Add("metricsSchemaVersion=$($script:startupMetricsSchemaVersion)")
$analysisContractLines.Add("analysisWindow=GameLayer::OnAttach.Start->GameLayer::StartupReady.End")
$analysisContractLines.Add("primaryMetric=Application::RuntimeStartupFirstPresent.End->Application::RuntimeStartupFirstGameplayPresent.End")
$analysisContractLines.Add("operationalReadyMetric=Application::RuntimeStartupFirstPresent.End->GameLayer::StartupReady.End")
$analysisContractLines.Add("completionTailMetric=Application::RuntimeStartupFirstGameplayPresent.End->GameLayer::StartupReady.End")
$analysisContractLines.Add("responsivenessProxy=TracyTrace.Start->Application::RuntimeStartupFirstPresent.End")
$analysisContractLines.Add("sectionThreadPolicy=any-thread-fully-inside-window")
$analysisContractLines.Add("sectionPolicy:partial-overlap=fail")
$analysisContractLines.Add("sectionPolicy:outside-window=diagnostic")
$analysisContractLines.Add("warmupBuildStage=Application::RuntimeStartupFirstPresent.End<=EnvironmentSystem::WarmupStep.Start&&EnvironmentSystem::WarmupStep.End<=GameLayer::RuntimeStart.Start")
$analysisContractLines.Add("warmupExpectedCounts=build:$warmupBuildExpectedCount|analysisInside:$warmupInsideExpectedCount")
$analysisContractLines.Add("warmupBuildMetrics=count,total,max,p$warmupPercentile-nearest-rank-ceiling")
$analysisContractLines.Add("warmupThreadPolicy=analysis-inside-events-on-GameLayer::OnAttach.Thread")
$analysisContractLines.Add("warmupInsidePolicy=third-class,partial,off-main,or-stage-overlap-fail")
$analysisContractLines.Add("commitChildren=AcquireCubeMapLibrary,CacheInstall,LibrarySet,ReleaseStaging|exactly-one-each|same-main-thread")
$analysisContractLines.Add("commitChildWindowPolicy=rawExact:1|analysisInsideExact:1|outside:0|partial:0")
$analysisContractLines.Add("commitParent=unique-WarmupStep-containing-all-four-children|chronologicalSort=StartNs,EndNs|expectedBuildOrdinal:$commitExpectedParentOrdinal")
$analysisContractLines.Add("commitMetrics=parentDuration,childTotal,unattributed|unattributed>=0")
$analysisContractLines.Add("legacyPolicy=reject-missing-v4-async-anchors-or-environment-sections")
$analysisContractLines.Add("anchorThreadPolicy=all-required-anchors-on-one-main-thread")
foreach ($zoneKey in $zoneDefinitions.Keys) {
    $analysisContractLines.Add("section:$zoneKey=$([string]$zoneDefinitions[$zoneKey].exactName)")
}
foreach ($anchorKey in $anchorDefinitions.Keys) {
    $analysisContractLines.Add("anchor:$anchorKey=$([string]$anchorDefinitions[$anchorKey].exactName)")
}
foreach ($anchorName in $requiredStartupAnchorNames) {
    $analysisContractLines.Add("requiredAnchor:$anchorName=exactly-one")
}
$analysisContractLines.Add("anchorOrder:GameLayer::OnAttach.End<=Application::RuntimeStartupFirstPresent.Start")
$analysisContractLines.Add("anchorOrder:Application::RuntimeStartupFirstPresent.End<=Application::RuntimeStartupFirstGameplayPresent.Start")
$analysisContractLines.Add("anchorOrder:Application::RuntimeStartupFirstGameplayPresent.End<=GameLayer::StartupReady.Start")
foreach ($relationship in $containmentRelationships) {
    $analysisContractLines.Add(
        "containment:$([string]$relationship.Name)|parent=$([string]$relationship.Parent)|child=$([string]$relationship.Child)|sameThread=$([bool]$relationship.RequireSameThread)")
}
foreach ($relationship in $orderRelationships) {
    $analysisContractLines.Add(
        "order:$([string]$relationship.Name)|before=$([string]$relationship.Before)|after=$([string]$relationship.After)")
}
foreach ($relationship in $commitOrderRelationships) {
    $analysisContractLines.Add(
        "commitOrder:$([string]$relationship.Name)|before=$([string]$relationship.Before)|after=$([string]$relationship.After)|nonOverlapping=true")
}
$analysisContractSha256 = Get-TextSha256 -Value ($analysisContractLines.ToArray() -join "`n")
$zoneCsvPaths = [ordered]@{}
$zoneStderrPaths = [ordered]@{}
foreach ($zoneKey in $zoneDefinitions.Keys) {
    $fileStem = [string]$zoneDefinitions[$zoneKey].fileStem
    $zoneCsvPaths[$zoneKey] = Join-Path $outputDirectory "$fileStem-events.csv"
    $zoneStderrPaths[$zoneKey] = Join-Path $outputDirectory "$fileStem-export.stderr.log"
}
$anchorCsvPaths = [ordered]@{}
$anchorStderrPaths = [ordered]@{}
foreach ($anchorKey in $anchorDefinitions.Keys) {
    $fileStem = [string]$anchorDefinitions[$anchorKey].fileStem
    $anchorCsvPaths[$anchorKey] = Join-Path $outputDirectory "$fileStem-events.csv"
    $anchorStderrPaths[$anchorKey] = Join-Path $outputDirectory "$fileStem-export.stderr.log"
}

$startedUtc = [DateTime]::UtcNow
$expectedArtifact = Join-Path $script:repositoryRoot (
    "out\agent\game-tracy-{0}\bin\StarbloomSurvivor.exe" -f $Configuration.ToLowerInvariant())
$ownerTag = [guid]::NewGuid().ToString("N")
$metadata = [ordered]@{
    schemaVersion = $script:startupMetadataSchemaVersion
    status = "running"
    mode = "startup-capture"
    analysis = [ordered]@{
        revision = $script:startupAnalysisRevision
        metricsSchemaVersion = $script:startupMetricsSchemaVersion
        contractSha256 = $analysisContractSha256
        compatibility = "pending"
        legacyTracePolicy = "reject traces missing the v4 startup anchors, EnvironmentSystem::WarmupStep, or any EnvironmentSystem::Commit child section"
        requiredAnchors = @($requiredStartupAnchorNames)
        environmentWarmup = [ordered]@{
            exactName = [string]$zoneDefinitions[$warmupStepKey].exactName
            expectedBuildCount = $warmupBuildExpectedCount
            expectedAnalysisInsideCount = $warmupInsideExpectedCount
            percentile = $warmupPercentile
            percentileMethod = "nearest-rank-ceiling"
        }
        environmentCommit = [ordered]@{
            childExactNames = @($commitSectionKeys | ForEach-Object {
                [string]$zoneDefinitions[$_].exactName
            })
            expectedChildCount = $commitExpectedChildCount
            expectedWarmupBuildOrdinal = $commitExpectedParentOrdinal
            parentSelection = "unique WarmupStep containing all four exact child intervals; build ordinal uses chronological StartNs, EndNs ordering"
            unattributedPolicy = "parent duration minus child total must be non-negative"
        }
    }
    tracyVersion = $script:tracyVersion
    startedUtc = $startedUtc.ToString("o")
    repositoryRoot = $script:repositoryRoot
    outputDirectory = $outputDirectory
    configuration = $Configuration
    resolution = $Resolution.ToLowerInvariant()
    fixedDeltaSeconds = $FixedDeltaSeconds
    ownerTag = $ownerTag
    timeouts = [ordered]@{
        buildSeconds = $BuildTimeoutSeconds
        agentReadyOrCallSeconds = $AgentTimeoutSeconds
        processBufferSeconds = $script:processTimeoutBufferSeconds
        agentStartProcessBudgetSeconds = $BuildTimeoutSeconds + $AgentTimeoutSeconds + `
            $script:processTimeoutBufferSeconds
        agentCallProcessBudgetSeconds = $AgentTimeoutSeconds + $script:processTimeoutBufferSeconds
        captureArmSeconds = $CaptureArmTimeoutSeconds
        captureMarkerDwellMilliseconds = $CaptureArmMilliseconds
        captureConnectSeconds = $CaptureConnectTimeoutSeconds
        captureShutdownSeconds = $CaptureShutdownTimeoutSeconds
        exportSeconds = $ExportTimeoutSeconds
    }
    script = Get-FileFingerprint -Path $PSCommandPath
    powershell = [ordered]@{
        edition = [string]$PSVersionTable.PSEdition
        version = $PSVersionTable.PSVersion.ToString()
    }
    captureOrder = @(
        "tracy-capture armed for 127.0.0.1:8086",
        "hectl agent start -EnableTracy",
        "Tracy connection verified by exact capture/game PIDs",
        "hectl agent world barrier",
        "hectl agent logs Game Ready evidence",
        "hectl agent stop",
        "tracy-capture exit and trace save",
        "exact-name CPU zone and startup-anchor export"
    )
    startupMetricInterpretation = "The primary user-visible startup metric spans Application::RuntimeStartupFirstPresent end through Application::RuntimeStartupFirstGameplayPresent end. FirstPresent end through StartupReady end is the secondary operational-ready metric, and FirstGameplayPresent end through StartupReady end is the completion tail. The section analysis window spans GameLayer::OnAttach start through GameLayer::StartupReady end and may cross frames and worker threads; it is not a parent CPU zone. Tracy trace start through RuntimeStartupFirstPresent end is only a responsiveness proxy because the trace clock has no verified UTC mapping to process start. Environment warmup metrics include exactly 68 pre-RuntimeStart build steps; any other in-window WarmupStep is invalid. Commit metrics identify the unique WarmupStep containing all four ordered Commit children and separate child total from non-negative unattributed parent time. Section totals are inclusive and are not added."
    requiredSections = @($zoneDefinitions.Values | ForEach-Object { [string]$_.exactName })
    tools = [ordered]@{
        capture = Get-FileFingerprint -Path $tracyCapturePath
        csvexport = Get-FileFingerprint -Path $tracyExporterPath
        agentScripts = [ordered]@{
            process = Get-FileFingerprint -Path $script:processScriptPath
            session = Get-FileFingerprint -Path $script:sessionScriptPath
            hectl = Get-FileFingerprint -Path $script:hectlScriptPath
            build = Get-FileFingerprint -Path $script:buildScriptPath
        }
    }
    artifacts = [ordered]@{
        trace = $tracePath
        rawCpuZoneEvents = $zoneCsvPaths
        rawStartupAnchorEvents = $anchorCsvPaths
        exactSectionEvents = $sectionEventsPath
        metrics = $metricsPath
        world = $worldPath
        logs = $logsPath
        gitStatus = $gitStatusPath
        hectlLog = $script:hectlLogPath
        captureStdout = $captureStdoutPath
        captureStderr = $captureStderrPath
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
$script:hectlTimings = New-Object 'System.Collections.Generic.List[object]'
$agentCallProcessTimeoutSeconds = $AgentTimeoutSeconds + $script:processTimeoutBufferSeconds
$agentStartProcessTimeoutSeconds = $BuildTimeoutSeconds + $AgentTimeoutSeconds + `
    $script:processTimeoutBufferSeconds
$bSucceeded = $false

try {
    $script:toolJob = New-AgentProcessJob

    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $gitCommand) {
        throw "git.exe is required to record the startup profile source state."
    }
    $gitHeadOutput = @(& $gitCommand.Source -C $script:repositoryRoot rev-parse HEAD 2>&1)
    if ($LASTEXITCODE -ne 0 -or $gitHeadOutput.Count -ne 1) {
        throw "git rev-parse HEAD failed while recording startup profile metadata."
    }
    $gitBranchOutput = @(& $gitCommand.Source -C $script:repositoryRoot branch --show-current 2>&1)
    if ($LASTEXITCODE -ne 0 -or $gitBranchOutput.Count -gt 1) {
        throw "git branch --show-current failed while recording startup profile metadata."
    }
    $gitStatusOutput = @(& $gitCommand.Source -C $script:repositoryRoot status --short --untracked-files=all 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git status --short failed while recording startup profile metadata."
    }
    $gitStatusText = @($gitStatusOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    Write-NewUtf8Text -Path $gitStatusPath -Contents $gitStatusText
    $metadata["git"] = [ordered]@{
        head = [string]$gitHeadOutput[0]
        branch = if ($gitBranchOutput.Count -eq 1) { [string]$gitBranchOutput[0] } else { $null }
        dirty = $gitStatusOutput.Count -gt 0
        statusEntryCount = $gitStatusOutput.Count
        statusPath = $gitStatusPath
        statusSha256 = (Get-FileHash -LiteralPath $gitStatusPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    $preexistingOwner = Read-ActiveSessionOptional
    $preexistingIdentity = Get-SessionIdentity -Owner $preexistingOwner
    if (Test-RecordedProcessAlive -Owner $preexistingOwner) {
        throw "An Agent session is already running (pid=$($preexistingOwner.pid)); startup profiling will not disturb it."
    }

    $existingCapture = Get-AgentArtifactProcess -ArtifactPath $tracyCapturePath `
        -ProcessName "tracy-capture"
    if ($null -ne $existingCapture) {
        throw "The repository Tracy capture tool is already running: pid=$($existingCapture.Id)."
    }
    $existingListeners = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object {
        [int]$_.LocalPort -eq $script:tracyPort
    })
    if ($existingListeners.Count -gt 0) {
        throw "TCP port $($script:tracyPort) already has a listener; refusing to capture an unrelated Tracy session."
    }

    Assert-NewFile -Path $tracePath
    $captureProcess = Start-OwnedProcess -FilePath $tracyCapturePath `
        -Arguments @("-o", $tracePath, "-a", $script:tracyAddress, "-p", ([string]$script:tracyPort)) `
        -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath
    $metadata["capture"] = [ordered]@{
        pid = $captureProcess.Id
        processStartUtc = $captureProcess.StartTime.ToUniversalTime().ToString("o")
        target = "$($script:tracyAddress):$($script:tracyPort)"
        armed = Wait-TracyCaptureArmed -Process $captureProcess `
            -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath
    }

    $fixedDeltaText = $FixedDeltaSeconds.ToString(
        "R", [System.Globalization.CultureInfo]::InvariantCulture)
    $agentStartAttempted = $true
    $agentStartInvocationUtc = [DateTime]::UtcNow
    $startCall = Invoke-HeCtl -Label "Tracy startup Agent start" -Arguments @(
        "agent", "start",
        "-Configuration", $Configuration,
        "-Parallel", ([string]$Parallel),
        "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
        "-FixedDeltaSeconds", $fixedDeltaText,
        "-Resolution", $Resolution,
        "-EnableTracy",
        "-AgentOwnerTag", $ownerTag
    ) -ProcessTimeoutSeconds $agentStartProcessTimeoutSeconds
    $startedOwner = Read-ActiveSessionOptional
    if ($null -eq $startedOwner) {
        throw "Agent start succeeded without publishing active.json."
    }
    Assert-OwnedReadySession -Owner $startedOwner -PreviousIdentity $preexistingIdentity `
        -ExpectedArtifact $expectedArtifact -ExpectedOwnerTag $ownerTag
    $sessionOwner = $startedOwner
    $sessionStarted = $true
    $gameProcess = Get-RecordedProcess -Owner $sessionOwner
    if ($null -eq $gameProcess) {
        throw "The recorded startup-profile HGame process is no longer alive."
    }
    if ($captureProcess.StartTime.ToUniversalTime() -gt $gameProcess.StartTime.ToUniversalTime()) {
        throw "tracy-capture did not start before the profiled HGame process."
    }
    $metadata["session"] = [ordered]@{
        pid = [int]$sessionOwner.pid
        processStartUtc = $gameProcess.StartTime.ToUniversalTime().ToString("o")
        processStartUtcTicks = [long]$sessionOwner.processStartUtcTicks
        runDirectory = [string]$sessionOwner.runDirectory
        artifact = [string]$sessionOwner.artifact
        artifactLength = (Get-Item -LiteralPath ([string]$sessionOwner.artifact)).Length
        artifactSha256 = (Get-FileHash -LiteralPath ([string]$sessionOwner.artifact) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        tracyEnabled = [bool]$sessionOwner.tracyEnabled
        ownerTag = [string]$sessionOwner.ownerTag
    }
    $metadata.capture["connection"] = Wait-TracyCaptureConnection `
        -Process $captureProcess -GamePid ([int]$sessionOwner.pid) `
        -GameStartUtcTicks ([long]$sessionOwner.processStartUtcTicks) `
        -StdoutPath $captureStdoutPath -StderrPath $captureStderrPath

    # world.summary is handled on the first Agent pre-update after StartupReady;
    # its response is the reliable barrier that the published scene is queryable.
    $worldCall = Invoke-HeCtl -Label "Tracy startup world barrier" -Arguments @(
        "agent", "world",
        "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
        "-AgentOwnerTag", $ownerTag
    ) -ProcessTimeoutSeconds $agentCallProcessTimeoutSeconds
    $worldEnvelope = ConvertFrom-HeCtlEnvelope -CallResult $worldCall -Label "agent world"
    if (-not [bool]$worldEnvelope.result.available) {
        throw "agent world did not report an available world."
    }
    Write-NewJson -Path $worldPath -Value $worldEnvelope

    $logsCall = Invoke-HeCtl -Label "Tracy startup Game Ready logs" -Arguments @(
        "agent", "logs",
        "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
        "-AgentOwnerTag", $ownerTag
    ) -ProcessTimeoutSeconds $agentCallProcessTimeoutSeconds
    $logsEnvelope = ConvertFrom-HeCtlEnvelope -CallResult $logsCall -Label "agent logs"
    $readyEntries = @($logsEnvelope.result.entries | Where-Object {
        ([string]$_.message).StartsWith("[Game] Ready ", [System.StringComparison]::Ordinal)
    })
    if ($readyEntries.Count -eq 0) {
        throw "agent logs did not contain the required '[Game] Ready ' evidence."
    }
    $readyEntriesWithSource = @($readyEntries | Where-Object {
        $null -ne $_.PSObject.Properties["source"]
    })
    $sourceValidation = "unavailable"
    $acceptedReadyEntries = $readyEntries
    if ($readyEntriesWithSource.Count -gt 0) {
        $acceptedReadyEntries = @($readyEntriesWithSource | Where-Object {
            [string]::Equals([string]$_.source, "engine", [System.StringComparison]::Ordinal)
        })
        if ($acceptedReadyEntries.Count -eq 0) {
            throw "The '[Game] Ready ' log entries did not originate from the expected engine source."
        }
        $sourceValidation = "engine"
    }
    Write-NewJson -Path $logsPath -Value $logsEnvelope

    $processStartUtc = $gameProcess.StartTime.ToUniversalTime()
    $metadata["gameReadyEvidence"] = [ordered]@{
        worldBarrierCompletedUtc = $worldCall.CompletedUtc.ToString("o")
        processToWorldBarrierUpperBoundMilliseconds = [Math]::Round(
            ($worldCall.CompletedUtc - $processStartUtc).TotalMilliseconds, 3)
        logPrefix = "[Game] Ready "
        matchingLogCount = $readyEntries.Count
        acceptedSource = $sourceValidation
        acceptedLogCount = $acceptedReadyEntries.Count
        firstMatchingLogElapsedMilliseconds = [long]$acceptedReadyEntries[0].elapsedMs
        logsReadCompletedUtc = $logsCall.CompletedUtc.ToString("o")
    }

    $stopCall = Invoke-HeCtl -Label "Tracy startup Agent stop" -Arguments @(
        "agent", "stop",
        "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
        "-AgentOwnerTag", $ownerTag
    ) -ProcessTimeoutSeconds $agentCallProcessTimeoutSeconds
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
    $metadata.capture["saved"] = [ordered]@{
        exitCode = $captureProcess.ExitCode
        saveMarkerObserved = $saveMarkerObserved
        traceLength = (Get-Item -LiteralPath $tracePath).Length
        traceSha256 = (Get-FileHash -LiteralPath $tracePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if (-not $saveMarkerObserved) {
        throw "tracy-capture exited without the required 'Saving trace... done!' marker."
    }

    $sectionMetrics = [ordered]@{}
    $anchorMetrics = [ordered]@{}
    $sectionScans = @{}
    $anchorScans = @{}
    $sectionSelectionsByKey = @{}
    $containedIntervalsByKey = @{}
    $missingSectionDiagnostics = New-Object 'System.Collections.Generic.List[object]'
    $partialOverlapDiagnostics = New-Object 'System.Collections.Generic.List[object]'
    $warmupValidationFailures = New-Object 'System.Collections.Generic.List[object]'
    $commitValidationFailures = New-Object 'System.Collections.Generic.List[object]'
    $relationshipViolations = New-Object 'System.Collections.Generic.List[object]'
    $sectionEventRows = New-Object 'System.Collections.Generic.List[object]'
    $metadata["artifacts"] = [ordered]@{
        trace = $tracePath
        rawCpuZoneEvents = $zoneCsvPaths
        rawStartupAnchorEvents = $anchorCsvPaths
        exactSectionEvents = $sectionEventsPath
        metrics = $metricsPath
        world = $worldPath
        logs = $logsPath
        gitStatus = $gitStatusPath
        hectlLog = $script:hectlLogPath
        captureStdout = $captureStdoutPath
        captureStderr = $captureStderrPath
    }

    foreach ($zoneKey in $zoneDefinitions.Keys) {
        $zone = $zoneDefinitions[$zoneKey]
        Invoke-TracyExport -ExporterPath $tracyExporterPath `
            -Arguments @("-u", "-f", ([string]$zone.exactName), $tracePath) `
            -CsvPath ([string]$zoneCsvPaths[$zoneKey]) `
            -StderrPath ([string]$zoneStderrPaths[$zoneKey]) `
            -Label "Tracy startup CPU zone export: $zoneKey"
        $sectionScans[$zoneKey] = Read-ExactZoneIntervals `
            -CsvPath ([string]$zoneCsvPaths[$zoneKey]) `
            -ExactZoneName ([string]$zone.exactName)
    }
    foreach ($anchorKey in $anchorDefinitions.Keys) {
        $anchor = $anchorDefinitions[$anchorKey]
        Invoke-TracyExport -ExporterPath $tracyExporterPath `
            -Arguments @("-u", "-f", ([string]$anchor.exactName), $tracePath) `
            -CsvPath ([string]$anchorCsvPaths[$anchorKey]) `
            -StderrPath ([string]$anchorStderrPaths[$anchorKey]) `
            -Label "Tracy startup anchor export: $anchorKey"
        $anchorScans[$anchorKey] = Read-ExactZoneIntervals `
            -CsvPath ([string]$anchorCsvPaths[$anchorKey]) `
            -ExactZoneName ([string]$anchor.exactName)
    }

    $onAttachKey = "gameLayerOnAttach"
    $firstPresentKey = "runtimeStartupFirstPresent"
    $firstGameplayPresentKey = "runtimeStartupFirstGameplayPresent"
    $startupReadyKey = "gameLayerStartupReady"
    $onAttachIntervals = @($sectionScans[$onAttachKey].Intervals)
    $firstPresentIntervals = @($anchorScans[$firstPresentKey].Intervals)
    $firstGameplayPresentIntervals = @($anchorScans[$firstGameplayPresentKey].Intervals)
    $startupReadyIntervals = @($anchorScans[$startupReadyKey].Intervals)

    $onAttachAnchorMetrics = Get-SectionMetrics `
        -ExactZoneName ([string]$zoneDefinitions[$onAttachKey].exactName) `
        -Intervals $onAttachIntervals
    $onAttachAnchorMetrics["observedExactCount"] = $onAttachIntervals.Count
    $onAttachAnchorMetrics["rawCsv"] = [string]$zoneCsvPaths[$onAttachKey]
    $onAttachAnchorMetrics["exportedRowCount"] = [long]$sectionScans[$onAttachKey].ExportedRowCount
    $onAttachAnchorMetrics["exportedNames"] = @($sectionScans[$onAttachKey].ExportedNames)
    $onAttachAnchorMetrics["nearbyNames"] = @($sectionScans[$onAttachKey].NearbyNames)
    $anchorMetrics[$onAttachKey] = $onAttachAnchorMetrics
    foreach ($anchorKey in $anchorDefinitions.Keys) {
        $anchor = $anchorDefinitions[$anchorKey]
        $scan = $anchorScans[$anchorKey]
        $intervals = @($scan.Intervals)
        $metrics = Get-SectionMetrics -ExactZoneName ([string]$anchor.exactName) `
            -Intervals $intervals
        $metrics["observedExactCount"] = $intervals.Count
        $metrics["rawCsv"] = [string]$anchorCsvPaths[$anchorKey]
        $metrics["exportedRowCount"] = [long]$scan.ExportedRowCount
        $metrics["exportedNames"] = @($scan.ExportedNames)
        $metrics["nearbyNames"] = @($scan.NearbyNames)
        $anchorMetrics[$anchorKey] = $metrics
    }
    $metadata["startupAnchors"] = $anchorMetrics

    $anchorFailures = New-Object 'System.Collections.Generic.List[string]'
    $anchorCardinality = Get-AnchorCardinalityValidation -Requirements @(
        [pscustomobject]@{ Name = "GameLayer::OnAttach"; Count = $onAttachIntervals.Count; bAsync = $false },
        [pscustomobject]@{ Name = "Application::RuntimeStartupFirstPresent"; Count = $firstPresentIntervals.Count; bAsync = $true },
        [pscustomobject]@{ Name = "Application::RuntimeStartupFirstGameplayPresent"; Count = $firstGameplayPresentIntervals.Count; bAsync = $true },
        [pscustomobject]@{ Name = "GameLayer::StartupReady"; Count = $startupReadyIntervals.Count; bAsync = $true })
    foreach ($failure in @($anchorCardinality.Failures)) {
        $anchorFailures.Add([string]$failure)
    }
    $bMissingAsyncAnchor = [bool]$anchorCardinality.MissingAsyncAnchor

    if ($anchorFailures.Count -eq 0) {
        $onAttach = $onAttachIntervals[0]
        $firstPresent = $firstPresentIntervals[0]
        $firstGameplayPresent = $firstGameplayPresentIntervals[0]
        $startupReady = $startupReadyIntervals[0]
        if (-not [string]::Equals([string]$onAttach.Thread, [string]$firstPresent.Thread,
            [System.StringComparison]::Ordinal) -or
            -not [string]::Equals([string]$onAttach.Thread, [string]$firstGameplayPresent.Thread,
            [System.StringComparison]::Ordinal) -or
            -not [string]::Equals([string]$onAttach.Thread, [string]$startupReady.Thread,
            [System.StringComparison]::Ordinal)) {
            $anchorFailures.Add("OnAttach, RuntimeStartupFirstPresent, RuntimeStartupFirstGameplayPresent, and StartupReady anchors are not on one main thread")
        }
        if ($onAttach.EndNs -gt $firstPresent.StartNs) {
            $anchorFailures.Add("GameLayer::OnAttach overlaps or follows Application::RuntimeStartupFirstPresent")
        }
        if ($firstPresent.EndNs -gt $firstGameplayPresent.StartNs) {
            $anchorFailures.Add("Application::RuntimeStartupFirstPresent overlaps or follows Application::RuntimeStartupFirstGameplayPresent")
        }
        if ($firstGameplayPresent.EndNs -gt $startupReady.StartNs) {
            $anchorFailures.Add("Application::RuntimeStartupFirstGameplayPresent overlaps or follows GameLayer::StartupReady")
        }
    }

    if ($anchorFailures.Count -gt 0) {
        $compatibility = if ($bMissingAsyncAnchor) { "legacy-incompatible" } else { "incompatible" }
        $metadata.analysis.compatibility = $compatibility
        $metadata.analysis["validationStatus"] = "fail"
        $metadata.analysis["anchorFailures"] = $anchorFailures.ToArray()
        $metadata["sectionMetrics"] = [ordered]@{}
        $invalidWindowMetrics = [ordered]@{
            schemaVersion = $script:startupMetricsSchemaVersion
            analysisRevision = $script:startupAnalysisRevision
            analysisContractSha256 = $analysisContractSha256
            compatibility = $compatibility
            validationStatus = "fail"
            exactNameMatch = $true
            windowPolicy = "Requires unique ordered OnAttach, RuntimeStartupFirstPresent, RuntimeStartupFirstGameplayPresent, and StartupReady anchors. Traces missing a v4 async anchor are rejected as legacy-incompatible."
            containmentPolicy = "No startup section containment is analyzed until the async startup anchors satisfy the current revision."
            startupWindow = $null
            visibleGameplayWindow = $null
            operationalReadyWindow = $null
            startupCompletionTail = $null
            startupWallClock = $null
            environmentWarmup = $null
            environmentCommit = $null
            anchors = $anchorMetrics
            anchorFailures = $anchorFailures.ToArray()
            sections = [ordered]@{}
        }
        Write-NewJson -Path $metricsPath -Value $invalidWindowMetrics
        throw "Startup trace is $compatibility with analysis revision '$($script:startupAnalysisRevision)': $($anchorFailures.ToArray() -join '; ')."
    }

    $metadata.analysis.compatibility = "compatible"
    $onAttach = $onAttachIntervals[0]
    $firstPresent = $firstPresentIntervals[0]
    $firstGameplayPresent = $firstGameplayPresentIntervals[0]
    $startupReady = $startupReadyIntervals[0]
    $startupWindow = [pscustomobject]@{
        Name = "RuntimeStartupAnalysisWindow"
        Thread = [string]$onAttach.Thread
        StartNs = [long]$onAttach.StartNs
        EndNs = [long]$startupReady.EndNs
        DurationNs = [long]$startupReady.EndNs - [long]$onAttach.StartNs
    }

    foreach ($anchorEvent in @(
        [pscustomobject]@{ Key = $firstPresentKey; Intervals = $firstPresentIntervals },
        [pscustomobject]@{ Key = $firstGameplayPresentKey; Intervals = $firstGameplayPresentIntervals },
        [pscustomobject]@{ Key = $startupReadyKey; Intervals = $startupReadyIntervals })) {
        foreach ($interval in @($anchorEvent.Intervals)) {
            $sectionEventRows.Add([pscustomobject]@{
                sectionKey = $anchorEvent.Key
                classification = "startup-anchor"
                name = $interval.Name
                thread = $interval.Thread
                ns_since_start = $interval.StartNs
                exec_time_ns = $interval.DurationNs
                exec_time_ms = [Math]::Round(([double]$interval.DurationNs) / 1000000.0, 6)
            })
        }
    }

    foreach ($zoneKey in $zoneDefinitions.Keys) {
        $zone = $zoneDefinitions[$zoneKey]
        $scan = $sectionScans[$zoneKey]
        $observedIntervals = @($scan.Intervals)
        $selection = Select-StartupWindowIntervals `
            -StartupWindow $startupWindow -Intervals $observedIntervals
        $sectionSelectionsByKey[$zoneKey] = $selection
        $intervals = @($selection.Contained)
        $containedIntervalsByKey[$zoneKey] = $intervals
        $metrics = Get-SectionMetrics -ExactZoneName ([string]$zone.exactName) `
            -Intervals $intervals
        $metrics["observedExactCount"] = $observedIntervals.Count
        $metrics["diagnostics"] = [ordered]@{
            rawCsv = [string]$zoneCsvPaths[$zoneKey]
            exportedRowCount = [long]$scan.ExportedRowCount
            exportedNames = @($scan.ExportedNames)
            nearbyNames = @($scan.NearbyNames)
            insideWindowCount = @($selection.Contained).Count
            containedCount = @($selection.Contained).Count
            outsideWindowCount = @($selection.OutsideWindow).Count
            partialOverlapCount = @($selection.PartialOverlap).Count
            selectedThreads = @($intervals | ForEach-Object { [string]$_.Thread } | Sort-Object -Unique)
            excludedEvents = @(
                @(ConvertTo-IntervalEvidence -Intervals @($selection.OutsideWindow) `
                    -Classification "outside-window") +
                @(ConvertTo-IntervalEvidence -Intervals @($selection.PartialOverlap) `
                    -Classification "partial-overlap")
            )
        }
        if ($intervals.Count -eq 0) {
            $missingSectionDiagnostics.Add([ordered]@{
                sectionKey = $zoneKey
                zoneName = [string]$zone.exactName
                rawCsv = [string]$zoneCsvPaths[$zoneKey]
                exportedRowCount = [long]$scan.ExportedRowCount
                exportedNames = @($scan.ExportedNames)
                nearbyNames = @($scan.NearbyNames)
                observedExactCount = $observedIntervals.Count
                outsideWindowCount = @($selection.OutsideWindow).Count
                partialOverlapCount = @($selection.PartialOverlap).Count
            })
        }
        if (@($selection.PartialOverlap).Count -gt 0) {
            $partialOverlapDiagnostics.Add([ordered]@{
                sectionKey = $zoneKey
                zoneName = [string]$zone.exactName
                rawCsv = [string]$zoneCsvPaths[$zoneKey]
                events = @(ConvertTo-IntervalEvidence `
                    -Intervals @($selection.PartialOverlap) `
                    -Classification "partial-overlap")
            })
        }
        $sectionMetrics[$zoneKey] = $metrics

        foreach ($classification in @(
            [pscustomobject]@{ Name = "inside-window"; Values = @($selection.Contained) },
            [pscustomobject]@{ Name = "outside-window"; Values = @($selection.OutsideWindow) },
            [pscustomobject]@{ Name = "partial-overlap"; Values = @($selection.PartialOverlap) })) {
            foreach ($interval in @($classification.Values)) {
                $sectionEventRows.Add([pscustomobject]@{
                    sectionKey = $zoneKey
                    classification = $classification.Name
                    name = $interval.Name
                    thread = $interval.Thread
                    ns_since_start = $interval.StartNs
                    exec_time_ns = $interval.DurationNs
                    exec_time_ms = [Math]::Round(([double]$interval.DurationNs) / 1000000.0, 6)
                })
            }
        }
    }

    $bMissingV4EnvironmentSection = @($sectionScans[$warmupStepKey].Intervals).Count -eq 0 -or
        @($commitSectionKeys | Where-Object {
            @($sectionScans[$_].Intervals).Count -eq 0
        }).Count -gt 0
    $analysisCompatibility = if ($bMissingV4EnvironmentSection) {
        "legacy-incompatible"
    }
    else {
        "compatible"
    }
    $metadata.analysis.compatibility = $analysisCompatibility

    $warmupSelection = $sectionSelectionsByKey[$warmupStepKey]
    $warmupInsideIntervals = @($containedIntervalsByKey[$warmupStepKey])
    $runtimeStartIntervals = @($containedIntervalsByKey["gameLayerRuntimeStart"])
    $warmupBuildIntervals = @()
    if (@($warmupSelection.PartialOverlap).Count -gt 0) {
        $warmupValidationFailures.Add([ordered]@{
            classification = "partial-analysis-window"
            events = @(ConvertTo-IntervalEvidence `
                -Intervals @($warmupSelection.PartialOverlap) `
                -Classification "partial-analysis-window")
        })
    }
    foreach ($interval in $warmupInsideIntervals) {
        if (-not [string]::Equals(
            [string]$interval.Thread,
            [string]$onAttach.Thread,
            [System.StringComparison]::Ordinal)) {
            $evidence = @(ConvertTo-IntervalEvidence `
                -Intervals @($interval) -Classification "warmup-step")
            $warmupValidationFailures.Add([ordered]@{
                classification = "off-main-thread"
                expectedThread = [string]$onAttach.Thread
                event = $evidence[0]
            })
        }
    }

    if ($runtimeStartIntervals.Count -ne 1) {
        $warmupValidationFailures.Add([ordered]@{
            classification = "invalid-stage-boundary"
            requirement = "exactly one in-window GameLayer::RuntimeStart"
            observedCount = $runtimeStartIntervals.Count
        })
    }
    else {
        $runtimeStart = $runtimeStartIntervals[0]
        $warmupStageSelection = Select-EnvironmentWarmupStageIntervals `
            -Intervals $warmupInsideIntervals `
            -FirstPresent $firstPresent `
            -RuntimeStart $runtimeStart
        $warmupBuildIntervals = @($warmupStageSelection.Build)
        foreach ($interval in @($warmupStageSelection.Invalid)) {
            $evidence = @(ConvertTo-IntervalEvidence `
                -Intervals @($interval) -Classification "warmup-step")
            $warmupValidationFailures.Add([ordered]@{
                classification = "invalid-stage-or-overlap"
                buildStage = "RuntimeStartupFirstPresent.End<=Start and End<=GameLayer::RuntimeStart.Start"
                event = $evidence[0]
            })
        }
    }

    if ($warmupInsideIntervals.Count -ne $warmupInsideExpectedCount) {
        $warmupValidationFailures.Add([ordered]@{
            classification = "analysis-inside-count-mismatch"
            expectedCount = $warmupInsideExpectedCount
            observedCount = $warmupInsideIntervals.Count
        })
    }
    if ($warmupBuildIntervals.Count -ne $warmupBuildExpectedCount) {
        $warmupValidationFailures.Add([ordered]@{
            classification = "build-count-mismatch"
            expectedCount = $warmupBuildExpectedCount
            observedCount = $warmupBuildIntervals.Count
        })
    }
    $warmupBuildMetrics = Get-SectionMetrics `
        -ExactZoneName ([string]$zoneDefinitions[$warmupStepKey].exactName) `
        -Intervals $warmupBuildIntervals
    [long]$warmupP95Nanoseconds = Get-NearestRankPercentileNanoseconds `
        -Intervals $warmupBuildIntervals -Percentile $warmupPercentile
    $warmupBuildMetrics["p95Nanoseconds"] = $warmupP95Nanoseconds
    $warmupBuildMetrics["p95Milliseconds"] = [Math]::Round(
        ([double]$warmupP95Nanoseconds) / 1000000.0,
        6)
    $warmupMetrics = $sectionMetrics[$warmupStepKey]
    foreach ($metricName in @(
        "count", "totalNanoseconds", "totalMilliseconds", "maxNanoseconds",
        "maxMilliseconds", "firstStartNanoseconds", "lastEndNanoseconds")) {
        $warmupMetrics[$metricName] = $warmupBuildMetrics[$metricName]
    }
    $warmupMetrics["p95Nanoseconds"] = $warmupBuildMetrics.p95Nanoseconds
    $warmupMetrics["p95Milliseconds"] = $warmupBuildMetrics.p95Milliseconds
    $warmupMetrics["aggregationScope"] = "pre-RuntimeStart build stage only"
    $warmupMetrics["percentileMethod"] = "nearest-rank-ceiling"
    $warmupMetrics["expectedBuildCount"] = $warmupBuildExpectedCount
    $warmupMetrics["rawAnalysisInsideCount"] = $warmupInsideIntervals.Count
    $warmupMetrics["expectedAnalysisInsideCount"] = $warmupInsideExpectedCount
    $warmupMetrics["outsideWindowDiagnostics"] = [ordered]@{
        classification = "outside-analysis-window"
        count = @($warmupSelection.OutsideWindow).Count
        events = @(ConvertTo-IntervalEvidence `
            -Intervals @($warmupSelection.OutsideWindow) `
            -Classification "outside-analysis-window")
    }
    $warmupMetrics["stageValidation"] = [ordered]@{
        passed = $warmupValidationFailures.Count -eq 0
        failureCount = $warmupValidationFailures.Count
        failures = $warmupValidationFailures.ToArray()
    }

    $commitIntervalsByKey = [ordered]@{}
    $commitAllInsideIntervals = New-Object 'System.Collections.Generic.List[object]'
    $bCommitChildrenStructurallyValid = $true
    foreach ($commitKey in $commitSectionKeys) {
        $observed = @($sectionScans[$commitKey].Intervals)
        $inside = @($containedIntervalsByKey[$commitKey])
        $selection = $sectionSelectionsByKey[$commitKey]
        $commitIntervalsByKey[$commitKey] = $inside
        if ($observed.Count -ne $commitExpectedChildCount) {
            $bCommitChildrenStructurallyValid = $false
            $commitValidationFailures.Add([ordered]@{
                classification = "child-raw-count-mismatch"
                sectionKey = $commitKey
                expectedCount = $commitExpectedChildCount
                observedCount = $observed.Count
            })
        }
        if ($inside.Count -ne $commitExpectedChildCount) {
            $bCommitChildrenStructurallyValid = $false
            $commitValidationFailures.Add([ordered]@{
                classification = "child-inside-count-mismatch"
                sectionKey = $commitKey
                expectedCount = $commitExpectedChildCount
                observedCount = $inside.Count
            })
        }
        if (@($selection.OutsideWindow).Count -gt 0) {
            $bCommitChildrenStructurallyValid = $false
            $commitValidationFailures.Add([ordered]@{
                classification = "child-outside-analysis-window"
                sectionKey = $commitKey
                events = @(ConvertTo-IntervalEvidence `
                    -Intervals @($selection.OutsideWindow) `
                    -Classification "outside-analysis-window")
            })
        }
        if (@($selection.PartialOverlap).Count -gt 0) {
            $bCommitChildrenStructurallyValid = $false
            $commitValidationFailures.Add([ordered]@{
                classification = "child-partial-analysis-window"
                sectionKey = $commitKey
                events = @(ConvertTo-IntervalEvidence `
                    -Intervals @($selection.PartialOverlap) `
                    -Classification "partial-analysis-window")
            })
        }
        foreach ($interval in $inside) {
            $commitAllInsideIntervals.Add($interval)
            if (-not [string]::Equals(
                [string]$interval.Thread,
                [string]$onAttach.Thread,
                [System.StringComparison]::Ordinal)) {
                $bCommitChildrenStructurallyValid = $false
                $evidence = @(ConvertTo-IntervalEvidence `
                    -Intervals @($interval) -Classification "commit-child")
                $commitValidationFailures.Add([ordered]@{
                    classification = "child-off-main-thread"
                    sectionKey = $commitKey
                    expectedThread = [string]$onAttach.Thread
                    event = $evidence[0]
                })
            }
        }
    }

    $commitParentMatches = @()
    if ($bCommitChildrenStructurallyValid) {
        $commitParentMatches = @(Select-CommonContainingIntervals `
            -ParentIntervals $warmupBuildIntervals `
            -ChildIntervals $commitAllInsideIntervals.ToArray() `
            -RequireSameThread $true)
    }
    if ($commitParentMatches.Count -ne 1) {
        $commitValidationFailures.Add([ordered]@{
            classification = "commit-parent-count-mismatch"
            requirement = "unique WarmupStep containing all four Commit children"
            observedCount = $commitParentMatches.Count
        })
    }

    $commitParent = $null
    $commitParentOrdinal = $null
    $commitParentDurationNanoseconds = $null
    $commitChildTotalNanoseconds = $null
    $commitUnattributedNanoseconds = $null
    if ($commitParentMatches.Count -eq 1) {
        $commitParent = $commitParentMatches[0]
        $commitParentDurationNanoseconds = [long]$commitParent.DurationNs
        $orderedWarmupBuildIntervals = @($warmupBuildIntervals | Sort-Object StartNs, EndNs)
        for ($index = 0; $index -lt $orderedWarmupBuildIntervals.Count; ++$index) {
            if ([object]::ReferenceEquals($orderedWarmupBuildIntervals[$index], $commitParent)) {
                $commitParentOrdinal = $index + 1
                break
            }
        }
        if ($commitParentOrdinal -ne $commitExpectedParentOrdinal) {
            $commitValidationFailures.Add([ordered]@{
                classification = "commit-parent-ordinal-mismatch"
                expectedOrdinal = $commitExpectedParentOrdinal
                observedOrdinal = $commitParentOrdinal
            })
        }

        [long]$childTotal = 0
        $bChildTotalOverflow = $false
        foreach ($commitKey in $commitSectionKeys) {
            [long]$duration = [long]$commitIntervalsByKey[$commitKey][0].DurationNs
            if ($childTotal -gt [long]::MaxValue - $duration) {
                $bChildTotalOverflow = $true
                $commitValidationFailures.Add([ordered]@{
                    classification = "commit-child-total-overflow"
                })
                break
            }
            $childTotal += $duration
        }
        if (-not $bChildTotalOverflow) {
            $commitChildTotalNanoseconds = $childTotal
            $commitUnattributedNanoseconds =
                [long]$commitParent.DurationNs - $commitChildTotalNanoseconds
            if ($commitUnattributedNanoseconds -lt 0) {
                $commitValidationFailures.Add([ordered]@{
                    classification = "commit-negative-unattributed-duration"
                    parentDurationNanoseconds = [long]$commitParent.DurationNs
                    childTotalNanoseconds = $commitChildTotalNanoseconds
                    unattributedNanoseconds = $commitUnattributedNanoseconds
                })
            }
        }
    }

    $commitOrderViolations = New-Object 'System.Collections.Generic.List[object]'
    foreach ($relationship in $commitOrderRelationships) {
        foreach ($violation in @(Get-IntervalOrderViolation `
            -Relationship ([string]$relationship.Name) `
            -BeforeIntervals @($commitIntervalsByKey[[string]$relationship.Before]) `
            -AfterIntervals @($commitIntervalsByKey[[string]$relationship.After]))) {
            $commitOrderViolations.Add($violation)
            $commitValidationFailures.Add([ordered]@{
                classification = "commit-child-order-overlap"
                violation = $violation
            })
        }
    }

    foreach ($relationship in $containmentRelationships) {
        foreach ($violation in @(Get-IntervalContainmentViolations `
            -Relationship ([string]$relationship.Name) `
            -ParentIntervals @($containedIntervalsByKey[[string]$relationship.Parent]) `
            -ChildIntervals @($containedIntervalsByKey[[string]$relationship.Child]) `
            -RequireSameThread ([bool]$relationship.RequireSameThread))) {
            $relationshipViolations.Add($violation)
        }
    }

    $relationshipIntervals = @{}
    foreach ($zoneKey in $zoneDefinitions.Keys) {
        $relationshipIntervals[$zoneKey] = @($containedIntervalsByKey[$zoneKey])
    }
    $relationshipIntervals[$firstPresentKey] = $firstPresentIntervals
    $relationshipIntervals[$firstGameplayPresentKey] = $firstGameplayPresentIntervals
    $relationshipIntervals[$startupReadyKey] = $startupReadyIntervals
    foreach ($relationship in $orderRelationships) {
        foreach ($violation in @(Get-IntervalOrderViolation `
            -Relationship ([string]$relationship.Name) `
            -BeforeIntervals @($relationshipIntervals[[string]$relationship.Before]) `
            -AfterIntervals @($relationshipIntervals[[string]$relationship.After]))) {
            $relationshipViolations.Add($violation)
        }
    }

    if ($sectionEventRows.Count -gt 0) {
        Assert-NewFile -Path $sectionEventsPath
        $sectionEventRows.ToArray() | Sort-Object ns_since_start | Export-Csv `
            -LiteralPath $sectionEventsPath -NoTypeInformation -Encoding UTF8
    }

    [long]$visibleGameplayNanoseconds =
        [long]$firstGameplayPresent.EndNs - [long]$firstPresent.EndNs
    $visibleGameplayMilliseconds = [Math]::Round(
        ([double]$visibleGameplayNanoseconds) / 1000000.0,
        6)
    [long]$operationalReadyNanoseconds =
        [long]$startupReady.EndNs - [long]$firstPresent.EndNs
    $operationalReadyMilliseconds = [Math]::Round(
        ([double]$operationalReadyNanoseconds) / 1000000.0,
        6)
    [long]$startupCompletionTailNanoseconds =
        [long]$startupReady.EndNs - [long]$firstGameplayPresent.EndNs
    $startupCompletionTailMilliseconds = [Math]::Round(
        ([double]$startupCompletionTailNanoseconds) / 1000000.0,
        6)
    [long]$traceStartToFirstPresentNanoseconds = [long]$firstPresent.EndNs
    $traceStartToFirstPresentMilliseconds = [Math]::Round(
        ([double]$traceStartToFirstPresentNanoseconds) / 1000000.0,
        6)
    $startupWindowDocument = [ordered]@{
        kind = "analysis-global-timestamp-window"
        purpose = "Select and validate all startup sections, including pre-Present bootstrap work."
        threadPolicy = "anchors share one main thread; sections may run on any thread"
        thread = [string]$onAttach.Thread
        mainThread = [string]$onAttach.Thread
        startAnchor = "GameLayer::OnAttach.Start"
        endAnchor = "GameLayer::StartupReady.End"
        startNanoseconds = $startupWindow.StartNs
        endNanoseconds = $startupWindow.EndNs
        durationNanoseconds = $startupWindow.DurationNs
    }
    $visibleGameplayWindow = [ordered]@{
        metricName = "FirstPresentToFirstGameplayPresent"
        kind = "user-visible-gameplay-timestamp-window"
        authority = "primary"
        thread = [string]$firstPresent.Thread
        mainThread = [string]$firstPresent.Thread
        startAnchor = "Application::RuntimeStartupFirstPresent.End"
        endAnchor = "Application::RuntimeStartupFirstGameplayPresent.End"
        startNanoseconds = [long]$firstPresent.EndNs
        endNanoseconds = [long]$firstGameplayPresent.EndNs
        durationNanoseconds = $visibleGameplayNanoseconds
        durationMilliseconds = $visibleGameplayMilliseconds
    }
    $operationalReadyWindow = [ordered]@{
        metricName = "FirstPresentToStartupReady"
        kind = "operational-ready-timestamp-window"
        authority = "secondary"
        thread = [string]$firstPresent.Thread
        mainThread = [string]$firstPresent.Thread
        startAnchor = "Application::RuntimeStartupFirstPresent.End"
        endAnchor = "GameLayer::StartupReady.End"
        startNanoseconds = [long]$firstPresent.EndNs
        endNanoseconds = [long]$startupReady.EndNs
        durationNanoseconds = $operationalReadyNanoseconds
        durationMilliseconds = $operationalReadyMilliseconds
    }
    $startupCompletionTail = [ordered]@{
        metricName = "FirstGameplayPresentToStartupReady"
        kind = "startup-completion-tail"
        startAnchor = "Application::RuntimeStartupFirstGameplayPresent.End"
        endAnchor = "GameLayer::StartupReady.End"
        startNanoseconds = [long]$firstGameplayPresent.EndNs
        endNanoseconds = [long]$startupReady.EndNs
        durationNanoseconds = $startupCompletionTailNanoseconds
        durationMilliseconds = $startupCompletionTailMilliseconds
    }
    $traceStartToFirstPresent = [ordered]@{
        metricName = "TraceStartToFirstPresent"
        startAnchor = "TracyTrace.Start"
        endAnchor = "Application::RuntimeStartupFirstPresent.End"
        durationNanoseconds = $traceStartToFirstPresentNanoseconds
        durationMilliseconds = $traceStartToFirstPresentMilliseconds
        interpretation = "Tracy trace-start proxy to the first successful Present. This is not labeled process-start time because the trace clock has no verified UTC mapping to the process creation timestamp."
    }
    $startupWallClock = [ordered]@{
        primaryMetricName = "FirstPresentToFirstGameplayPresent"
        primaryZoneName = "virtual:Application::RuntimeStartupFirstPresent.End->Application::RuntimeStartupFirstGameplayPresent.End"
        virtualWindow = $true
        interpretation = "Global Tracy timestamp span from the first successful startup Present through the first successfully presented gameplay frame. Operational readiness and its post-gameplay-Present completion tail are reported separately."
        count = 1
        totalNanoseconds = $visibleGameplayNanoseconds
        totalMilliseconds = $visibleGameplayMilliseconds
        maxNanoseconds = $visibleGameplayNanoseconds
        maxMilliseconds = $visibleGameplayMilliseconds
    }
    $relationshipValidation = [ordered]@{
        policy = "Only true local same-thread nesting and global stage order are enforced; worker sections are not required to be children of OnAttach or to share its thread."
        containmentChecks = @($containmentRelationships | ForEach-Object { [string]$_.Name })
        orderChecks = @($orderRelationships | ForEach-Object { [string]$_.Name })
        passed = $relationshipViolations.Count -eq 0
        violationCount = $relationshipViolations.Count
        violations = $relationshipViolations.ToArray()
    }
    $warmupValidation = [ordered]@{
        exactName = [string]$zoneDefinitions[$warmupStepKey].exactName
        policy = "Require exactly 68 pre-RuntimeStart build steps; reject every other in-window, partial, overlapping, or off-main WarmupStep. Outside-window events remain diagnostics."
        mainThread = [string]$onAttach.Thread
        expectedBuildCount = $warmupBuildExpectedCount
        expectedAnalysisInsideCount = $warmupInsideExpectedCount
        passed = $warmupValidationFailures.Count -eq 0
        failureCount = $warmupValidationFailures.Count
        failures = $warmupValidationFailures.ToArray()
        outsideWindowDiagnostics = $warmupMetrics.outsideWindowDiagnostics
    }
    $commitChildMetrics = [ordered]@{}
    foreach ($commitKey in $commitSectionKeys) {
        $commitChildMetrics[$commitKey] = $sectionMetrics[$commitKey]
    }
    $commitParentEvidence = $null
    if ($null -ne $commitParent) {
        $evidence = @(ConvertTo-IntervalEvidence `
            -Intervals @($commitParent) -Classification "commit-warmup-parent")
        $commitParentEvidence = $evidence[0]
    }
    $commitValidation = [ordered]@{
        policy = "Each Commit child must occur exactly once on the startup main thread, in source order without overlap, inside the unique ordinal-68 WarmupStep parent selected by containment."
        expectedChildCount = $commitExpectedChildCount
        expectedWarmupBuildOrdinal = $commitExpectedParentOrdinal
        passed = $commitValidationFailures.Count -eq 0
        failureCount = $commitValidationFailures.Count
        failures = $commitValidationFailures.ToArray()
        parent = [ordered]@{
            evidence = $commitParentEvidence
            buildOrdinal = $commitParentOrdinal
            durationNanoseconds = $commitParentDurationNanoseconds
            durationMilliseconds = if ($null -ne $commitParentDurationNanoseconds) {
                [Math]::Round(([double]$commitParentDurationNanoseconds) / 1000000.0, 6)
            } else { $null }
        }
        childTotalNanoseconds = $commitChildTotalNanoseconds
        childTotalMilliseconds = if ($null -ne $commitChildTotalNanoseconds) {
            [Math]::Round(([double]$commitChildTotalNanoseconds) / 1000000.0, 6)
        } else { $null }
        unattributedNanoseconds = $commitUnattributedNanoseconds
        unattributedMilliseconds = if ($null -ne $commitUnattributedNanoseconds) {
            [Math]::Round(([double]$commitUnattributedNanoseconds) / 1000000.0, 6)
        } else { $null }
        children = $commitChildMetrics
        orderChecks = @($commitOrderRelationships | ForEach-Object { [string]$_.Name })
        orderViolations = $commitOrderViolations.ToArray()
    }
    $bAnalysisValid = $missingSectionDiagnostics.Count -eq 0 -and
        $partialOverlapDiagnostics.Count -eq 0 -and
        $relationshipViolations.Count -eq 0 -and
        $warmupValidationFailures.Count -eq 0 -and
        $commitValidationFailures.Count -eq 0
    $analysisValidationStatus = if ($bAnalysisValid) { "pass" } else { "fail" }
    $metadata.analysis["validationStatus"] = $analysisValidationStatus
    $metricsDocument = [ordered]@{
        schemaVersion = $script:startupMetricsSchemaVersion
        analysisRevision = $script:startupAnalysisRevision
        analysisContractSha256 = $analysisContractSha256
        compatibility = $analysisCompatibility
        validationStatus = $analysisValidationStatus
        exactNameMatch = $true
        windowPolicy = "All required exact-name sections are selected inside OnAttach.Start->StartupReady.End. The primary latency window is FirstPresent.End->FirstGameplayPresent.End; FirstPresent.End->StartupReady.End is secondary. Partial overlaps fail; outside-window events remain diagnostics."
        containmentPolicy = "Required sections are contained by the global analysis window, not by the OnAttach CPU interval; only explicit local relationships require same-thread nesting."
        startupWindow = $startupWindowDocument
        visibleGameplayWindow = $visibleGameplayWindow
        operationalReadyWindow = $operationalReadyWindow
        startupCompletionTail = $startupCompletionTail
        traceStartToFirstPresent = $traceStartToFirstPresent
        startupWallClock = $startupWallClock
        anchors = $anchorMetrics
        relationships = $relationshipValidation
        environmentWarmup = $warmupValidation
        environmentCommit = $commitValidation
        missingSectionDiagnostics = $missingSectionDiagnostics.ToArray()
        partialOverlapDiagnostics = $partialOverlapDiagnostics.ToArray()
        sections = $sectionMetrics
    }
    Write-NewJson -Path $metricsPath -Value $metricsDocument
    $metadata["startupWindow"] = $startupWindowDocument
    $metadata["visibleGameplayWindow"] = $visibleGameplayWindow
    $metadata["operationalReadyWindow"] = $operationalReadyWindow
    $metadata["startupCompletionTail"] = $startupCompletionTail
    $metadata["traceStartToFirstPresent"] = $traceStartToFirstPresent
    $metadata["startupWallClock"] = $startupWallClock
    $metadata["sectionMetrics"] = $sectionMetrics
    $metadata["relationshipValidation"] = $relationshipValidation
    $metadata["environmentWarmupValidation"] = $warmupValidation
    $metadata["environmentCommitValidation"] = $commitValidation
    $metadata["missingSectionDiagnostics"] = $missingSectionDiagnostics.ToArray()
    $metadata["partialOverlapDiagnostics"] = $partialOverlapDiagnostics.ToArray()

    if (-not $bAnalysisValid) {
        $missingNames = @($missingSectionDiagnostics | ForEach-Object { [string]$_.zoneName }) -join "; "
        $partialNames = @($partialOverlapDiagnostics | ForEach-Object { [string]$_.zoneName }) -join "; "
        throw "Startup analysis validation failed for revision '$($script:startupAnalysisRevision)' with compatibility '$analysisCompatibility'. Missing inside-window sections: $missingNames. Partial-overlap sections: $partialNames. Relationship violations: $($relationshipViolations.Count). Environment warmup violations: $($warmupValidationFailures.Count). Environment Commit violations: $($commitValidationFailures.Count). See metadata diagnostics and raw CSV artifacts."
    }

    $metadata["status"] = "pass"
    $bSucceeded = $true
}
catch {
    $runError = $_
    $metadata["status"] = "fail"
    $metadata["error"] = $_.Exception.Message
    $metadata["errorPosition"] = $_.InvocationInfo.PositionMessage
    $metadata["scriptStackTrace"] = $_.ScriptStackTrace
    $metadata["errorLogs"] = [ordered]@{
        lastHeCtlStdout = $script:lastHeCtlStdoutPath
        lastHeCtlStderr = $script:lastHeCtlStderrPath
        hectl = $script:hectlLogPath
        captureStdout = $captureStdoutPath
        captureStderr = $captureStderrPath
    }
}
finally {
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
            $currentOwner = Read-ActiveSessionOptional
            $bSessionStillCurrent = $null -ne $currentOwner -and
                [string]::Equals(
                    (Get-SessionIdentity -Owner $currentOwner),
                    (Get-SessionIdentity -Owner $sessionOwner),
                    [System.StringComparison]::Ordinal) -and
                $null -ne $currentOwner.PSObject.Properties["ownerTag"] -and
                [string]::Equals(
                    [string]$currentOwner.ownerTag,
                    $ownerTag,
                    [System.StringComparison]::Ordinal)
        }
        catch {
            $cleanupErrors.Add("Failed to verify the current Agent owner before cleanup: $($_.Exception.Message)")
        }

        if ($bSessionStillCurrent) {
            try {
                $cleanupStop = Invoke-HeCtl -Label "Tracy startup Agent cleanup stop" -Arguments @(
                    "agent", "stop",
                    "-TimeoutSeconds", ([string]$AgentTimeoutSeconds),
                    "-AgentOwnerTag", $ownerTag
                ) -ProcessTimeoutSeconds $agentCallProcessTimeoutSeconds
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

    $recordedProcess = Get-RecordedProcess -Owner $sessionOwner
    if ($null -ne $recordedProcess) {
        try {
            Stop-AgentOwnedProcess -Process $recordedProcess `
                -Description "Tracy startup Agent game cleanup"
        }
        catch {
            $cleanupErrors.Add("Agent process cleanup failed: $($_.Exception.Message)")
        }
    }
    if (Test-RecordedProcessAlive -Owner $sessionOwner) {
        $cleanupErrors.Add("Agent game process remained alive after exact-process cleanup: pid=$($sessionOwner.pid)")
    }

    if ($null -ne $captureProcess) {
        try {
            $captureProcess.Refresh()
            if (-not $captureProcess.HasExited) {
                if (-not $captureProcess.WaitForExit($CaptureShutdownTimeoutSeconds * 1000)) {
                    Stop-AgentOwnedProcess -Process $captureProcess `
                        -Description "tracy-capture startup cleanup"
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
            $cleanupErrors.Add("Tracy startup tool Job cleanup failed: $($_.Exception.Message)")
        }
    }
    if ($null -ne $profileLock) {
        $profileLock.Dispose()
    }

    $metadata["hectlTimings"] = $script:hectlTimings.ToArray()
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
    Write-Host "partial startup profile artifacts: $outputDirectory"
    $storageState = $(if ($cleanupErrors.Count -gt 0) { 'unknown' } else { 'failed' })
    exit 1
}

$visibleGameplayMilliseconds = [double]$metadata.visibleGameplayWindow.durationMilliseconds
$operationalReadyMilliseconds = [double]$metadata.operationalReadyWindow.durationMilliseconds
$completionTailMilliseconds = [double]$metadata.startupCompletionTail.durationMilliseconds
$warmupBuildCount = [long]$metadata.sectionMetrics[$warmupStepKey].count
$warmupBuildP95Milliseconds = [double]$metadata.sectionMetrics[$warmupStepKey].p95Milliseconds
$commitTotalMilliseconds = [double]$metadata.environmentCommitValidation.parent.durationMilliseconds
$commitAcquireMilliseconds = [double]$metadata.sectionMetrics.environmentCommitAcquireCubeMapLibrary.totalMilliseconds
$commitCacheMilliseconds = [double]$metadata.sectionMetrics.environmentCommitCacheInstall.totalMilliseconds
$commitSetMilliseconds = [double]$metadata.sectionMetrics.environmentCommitLibrarySet.totalMilliseconds
$commitReleaseMilliseconds = [double]$metadata.sectionMetrics.environmentCommitReleaseStaging.totalMilliseconds
$commitUnattributedMilliseconds = [double]$metadata.environmentCommitValidation.unattributedMilliseconds
Write-Host ("Tracy startup profile: PASS; FirstPresentToFirstGameplayPresent={0:n3} ms; FirstPresentToStartupReady={1:n3} ms; FirstGameplayPresentToStartupReady={2:n3} ms; WarmupBuildSteps={3}; WarmupBuildP95={4:n3} ms; CommitTotal={5:n3} ms; CommitAcquire={6:n3} ms; CommitCache={7:n3} ms; CommitSet={8:n3} ms; CommitRelease={9:n3} ms; CommitUnattributed={10:n3} ms; sections={11}; analysis={12}; contractSha256={13}" -f `
    $visibleGameplayMilliseconds, $operationalReadyMilliseconds,
    $completionTailMilliseconds, $warmupBuildCount, $warmupBuildP95Milliseconds,
    $commitTotalMilliseconds, $commitAcquireMilliseconds, $commitCacheMilliseconds,
    $commitSetMilliseconds, $commitReleaseMilliseconds, $commitUnattributedMilliseconds,
    $zoneDefinitions.Count, $script:startupAnalysisRevision, $analysisContractSha256)
Write-Host "startup profile artifacts: $outputDirectory"
$storageState = 'success'
exit 0

}
finally {
    if ($profileLock) { $profileLock.Dispose() }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
