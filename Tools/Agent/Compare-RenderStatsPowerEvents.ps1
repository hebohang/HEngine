param(
    [Parameter(Mandatory=$true)][string]$RenderStatsPath,
    [double]$GpuThresholdMs = 33.333,
    [double]$EventPaddingSeconds = 2.0,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $RenderStatsPath).Path
$rows = @(Import-Csv -LiteralPath $source)
if ($rows.Count -eq 0) { throw "Render statistics CSV has no rows: $source" }
$culture = [System.Globalization.CultureInfo]::InvariantCulture

function Parse-Number([string]$value) {
    return [double]::Parse($value, $culture)
}

function Frame-Time([object]$row) {
    return [DateTimeOffset]::FromUnixTimeMilliseconds(
        [long][Math]::Floor((Parse-Number $row.cpuStartUtcMs))).LocalDateTime
}

$slow = @($rows | Where-Object {
    $_.gpuFrameSpanMs -and (Parse-Number $_.gpuFrameSpanMs) -gt $GpuThresholdMs
})
$bursts = [System.Collections.Generic.List[object]]::new()
$current = $null
foreach ($row in $slow) {
    $frame = [int64]$row.frame
    if ($null -eq $current -or $frame -ne $current.LastFrame + 1) {
        if ($null -ne $current) { $bursts.Add($current) }
        $current = [pscustomobject]@{
            FirstFrame = $frame
            LastFrame = $frame
            FirstTime = Frame-Time $row
            LastTime = Frame-Time $row
            FrameCount = 1
            MaxGpuMs = Parse-Number $row.gpuFrameSpanMs
        }
    } else {
        $current.LastFrame = $frame
        $current.LastTime = Frame-Time $row
        $current.FrameCount++
        $current.MaxGpuMs = [Math]::Max($current.MaxGpuMs,
            (Parse-Number $row.gpuFrameSpanMs))
    }
}
if ($null -ne $current) { $bursts.Add($current) }

$firstTime = Frame-Time $rows[0]
$lastTime = Frame-Time $rows[-1]
$padding = [TimeSpan]::FromSeconds($EventPaddingSeconds)
$filter = @{
    LogName='System'; Id=521
    StartTime=$firstTime.Subtract($padding)
    EndTime=$lastTime.Add($padding)
}
try {
    $rawEvents = @(Get-WinEvent -FilterHashtable $filter -ErrorAction Stop)
} catch {
    if ($_.FullyQualifiedErrorId -notmatch 'NoMatchingEventsFound') { throw }
    $rawEvents = @()
}
$powerEvents = @($rawEvents | Sort-Object TimeCreated | ForEach-Object {
    [pscustomobject]@{
        Time = $_.TimeCreated
        ValidBatteryCount = [int]$_.Properties[0].Value
    }
})

$burstReports = @($bursts | ForEach-Object {
    $burst = $_
    $nearby = @($powerEvents | Where-Object {
        $_.Time -ge $burst.FirstTime.Subtract($padding) -and
        $_.Time -le $burst.LastTime.Add($padding)
    })
    [pscustomobject]@{
        firstFrame = $burst.FirstFrame
        lastFrame = $burst.LastFrame
        frameCount = $burst.FrameCount
        firstTimeLocal = $burst.FirstTime.ToString('o')
        lastTimeLocal = $burst.LastTime.ToString('o')
        maxGpuMs = [Math]::Round($burst.MaxGpuMs, 3)
        nearbyBatteryCountEvents = @($nearby | ForEach-Object {
            [pscustomobject]@{
                timeLocal = $_.Time.ToString('o')
                validBatteryCount = $_.ValidBatteryCount
            }
        })
    }
})

$report = [pscustomobject]@{
    renderStatsPath = $source
    gpuThresholdMs = $GpuThresholdMs
    eventPaddingSeconds = $EventPaddingSeconds
    measuredFrames = $rows.Count
    gpuSlowFrames = $slow.Count
    gpuSlowBursts = $burstReports
    batteryCountEventsInRun = @($powerEvents | ForEach-Object {
        [pscustomobject]@{
            timeLocal = $_.Time.ToString('o')
            validBatteryCount = $_.ValidBatteryCount
        }
    })
    interpretation = 'Temporal correlation only. Battery count events do not prove the physical or driver cause of GPU clock changes.'
}
$json = $report | ConvertTo-Json -Depth 8
if ($OutputPath) {
    $target = [IO.Path]::GetFullPath($OutputPath)
    if ([IO.File]::Exists($target)) { throw "Refusing to overwrite report: $target" }
    [IO.File]::WriteAllText($target, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}
$json
