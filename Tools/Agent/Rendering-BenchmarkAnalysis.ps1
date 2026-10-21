function Get-RenderMemoryBudgetStatistics([string]$Path, [long]$FirstFrame, [long]$LastFrame) {
    $result = [ordered]@{ status='UNAVAILABLE'; source=$Path;
        interpretation='Once-per-second process budget observations, not frame-resolution residency or page-fault evidence. Warmup is excluded by frame number. OS budget can change independently of this process. Missing/unsupported data is not zero usage.' }
    if (!(Test-Path -LiteralPath $Path)) { return $result }
    $allRows = @(Import-Csv -LiteralPath $Path)
    $allProcessRows = @($allRows | Where-Object { $_.available -eq '1' -and $_.processScope -eq '1' })
    $result.allPhaseProcessObservations = $allProcessRows.Count
    if ($allProcessRows.Count -gt 0) {
        $result.allPhaseLocalUsagePeakBytes = ($allProcessRows | ForEach-Object { [uint64]$_.localUsageBytes } | Measure-Object -Maximum).Maximum
        $result.allPhaseOverBudgetObservations = @($allProcessRows | Where-Object { [uint64]$_.localUsageBytes -gt [uint64]$_.localBudgetBytes }).Count
    }
    if ($allRows.Count -gt 0 -and !$allRows[0].PSObject.Properties['automationFrame']) {
        $result.status='UNAVAILABLE_PHASE_TAG'; return $result
    }
    $rows = @($allRows | Where-Object {
        $_.automationFrame -eq '1' -and [long]$_.frame -ge $FirstFrame -and [long]$_.frame -le $LastFrame })
    $valid = @($rows | Where-Object { $_.available -eq '1' -and $_.processScope -eq '1' })
    $result.observations=$rows.Count; $result.processBudgetObservations=$valid.Count
    if ($valid.Count -eq 0) { $result.status='NO_PROCESS_BUDGET_OBSERVATIONS'; return $result }
    $result.status='OBSERVED'
    $result.overBudgetObservations=@($valid | Where-Object { [uint64]$_.localUsageBytes -gt [uint64]$_.localBudgetBytes }).Count
    foreach ($column in @('localBudgetBytes','localUsageBytes','localReservationBytes','queryMilliseconds')) {
        $values=@($valid | ForEach-Object { [double]::Parse($_.$column,[Globalization.CultureInfo]::InvariantCulture) })
        $result[$column]=Get-RenderSampleStatistics $values
    }
    $nonLocal=@($valid | Where-Object { $_.nonLocalAvailable -eq '1' })
    if ($nonLocal.Count -gt 0) {
        $result.nonLocalUsageBytes=Get-RenderSampleStatistics @($nonLocal | ForEach-Object { [double]$_.nonLocalUsageBytes })
    }
    return $result
}

function Get-RenderSampleStatistics([double[]]$Samples) {
    if ($Samples.Count -eq 0) { return @{ samples = 0 } }
    $ordered = @($Samples | Sort-Object)
    $percentiles = @{}
    foreach ($percentile in @(0.95, 0.99)) {
        $position = ($ordered.Count - 1) * $percentile
        $lower = [int][Math]::Floor($position)
        $upper = [int][Math]::Ceiling($position)
        $percentiles[$percentile] = $ordered[$lower] + ($ordered[$upper] - $ordered[$lower]) * ($position - $lower)
    }
    return [ordered]@{ samples = $Samples.Count; average = ($Samples | Measure-Object -Average).Average;
        p95 = $percentiles[0.95]; p99 = $percentiles[0.99]; minimum = $ordered[0]; maximum = $ordered[-1] }
}

function Get-RenderWorkloadStability([object[]]$Rows, [int]$PeriodFrames) {
    # Compare identical complete camera cycles. This does not discard a warm
    # looking suffix or relabel unexplained spikes as asset loading.
    $result = [ordered]@{ status='INSUFFICIENT_CAMERA_CYCLES'; measuredFrames=$Rows.Count;
        requiredFrames=2*$PeriodFrames; periodFrames=$PeriodFrames;
        interpretation='CPU/GPU drift is a workload-settling proxy, separate from thermal stability. All measured frames remain in budget/percentile statistics; spikes are unclassified without correlated loading evidence.' }
    if ($PeriodFrames -le 0 -or $Rows.Count -lt 2*$PeriodFrames) { return $result }
    $windows = @($Rows[0..($PeriodFrames-1)]),@($Rows[($Rows.Count-$PeriodFrames)..($Rows.Count-1)])
    $statistics = @()
    foreach ($window in $windows) {
        $cpu = @($window | ForEach-Object { [double]::Parse($_.cpuWallFrameMs,[Globalization.CultureInfo]::InvariantCulture) })
        $gpu = @($window | Where-Object { [uint64]$_.gpuSample -gt [uint64]$_.gpuWarmupCutoffSample -and [double]$_.gpuFrameSpanMs -gt 0 } |
            Group-Object gpuSample | ForEach-Object { [double]::Parse($_.Group[0].gpuFrameSpanMs,[Globalization.CultureInfo]::InvariantCulture) })
        $statistics += [ordered]@{ cpu=Get-RenderSampleStatistics $cpu; gpu=Get-RenderSampleStatistics $gpu;
            framesOver50Milliseconds=@($cpu | Where-Object { $_ -gt 50 }).Count;
            sceneTableUploadBytes=($window | ForEach-Object { [uint64]$_.sceneTableUploadedBytes } | Measure-Object -Sum).Sum }
    }
    $result.firstCycle=$statistics[0]; $result.lastCycle=$statistics[1]
    if ($statistics[0].gpu.samples -lt $PeriodFrames-8 -or $statistics[1].gpu.samples -lt $PeriodFrames-8) {
        $result.status='INSUFFICIENT_GPU_OBSERVATIONS'; return $result
    }
    $drift = [ordered]@{}
    foreach ($phase in @('cpu','gpu')) {
        $drift[$phase+'MeanRelative']=[Math]::Abs($statistics[0][$phase].average-$statistics[1][$phase].average)/[Math]::Max($statistics[1][$phase].average,1e-6)
        $drift[$phase+'P95Relative']=[Math]::Abs($statistics[0][$phase].p95-$statistics[1][$phase].p95)/[Math]::Max($statistics[1][$phase].p95,1e-6)
    }
    $result.drift=$drift
    $result.thresholds=@{ meanRelative=0.05; p95Relative=0.10; unexplainedFramesOver50Milliseconds=0 }
    $result.status=if ($drift.cpuMeanRelative -le .05 -and $drift.gpuMeanRelative -le .05 -and
        $drift.cpuP95Relative -le .10 -and $drift.gpuP95Relative -le .10 -and
        @($Rows | Where-Object { [double]$_.cpuWallFrameMs -gt 50 }).Count -eq 0) {
        'PASS_REPEAT_CYCLE_PROXY'
    } else { 'FAIL_NOT_SETTLED_OR_UNEXPLAINED_SPIKES' }
    return $result
}

function Get-RenderPassStatistics([string]$Path, [uint64[]]$SampleIds) {
    $ids = [Collections.Generic.HashSet[uint64]]::new()
    foreach ($sampleId in $SampleIds) { $null = $ids.Add($sampleId) }
    $seen = [Collections.Generic.HashSet[string]]::new()
    $passes = @{}
    $reader = [IO.File]::OpenText($Path)
    try {
        $header = $reader.ReadLine()
        $hasSubmission = $header -eq 'gpuSample,passName,milliseconds,graphSubmissionId'
        if (-not $hasSubmission -and $header -ne 'gpuSample,passName,milliseconds') { throw 'Unknown GPU pass CSV schema.' }
        $rowPattern = if ($hasSubmission) { '^(\d+),(?:"((?:[^"]|"")*)"|([^,]*)),([-+0-9.eE]+),(\d+)$' }
            else { '^(\d+),(?:"((?:[^"]|"")*)"|([^,]*)),([-+0-9.eE]+)$' }
        $submissionBySample = @{}
        $sampleBySubmission = @{}
        while ($null -ne ($line = $reader.ReadLine())) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            if ($line -notmatch $rowPattern) { throw 'Malformed GPU pass CSV row.' }
            $sampleId = [uint64]$Matches[1]
            if ($hasSubmission) {
                $submissionId = [uint64]$Matches[5]
                if ($submissionId -eq 0 -or
                    ($submissionBySample.ContainsKey($sampleId) -and $submissionBySample[$sampleId] -ne $submissionId) -or
                    ($sampleBySubmission.ContainsKey($submissionId) -and $sampleBySubmission[$submissionId] -ne $sampleId)) {
                    throw 'Conflicting GPU graph submission identity.'
                }
                $submissionBySample[$sampleId] = $submissionId
                $sampleBySubmission[$submissionId] = $sampleId
            }
            if (-not $ids.Contains($sampleId)) { continue }
            $name = if ($Matches[2]) { $Matches[2].Replace('""','"') } else { $Matches[3] }
            $time = [double]::Parse($Matches[4],[Globalization.CultureInfo]::InvariantCulture)
            if ([double]::IsNaN($time) -or [double]::IsInfinity($time) -or $time -lt 0) { throw 'Invalid GPU pass duration.' }
            if ($name -eq 'FrameGpuSpan') { continue }
            if (-not $seen.Add("$sampleId|$name")) { throw 'GPU pass sample duplicated in export.' }
            if (-not $passes.ContainsKey($name)) { $passes[$name] = [Collections.Generic.List[double]]::new() }
            $passes[$name].Add($time)
        }
    } finally { $reader.Dispose() }
    $result = [ordered]@{}
    foreach ($name in ($passes.Keys | Sort-Object)) { $result[$name] = Get-RenderSampleStatistics -Samples $passes[$name].ToArray() }
    return $result
}

function Get-RenderThermalStability([string]$Path, [int]$WindowSeconds, [DateTime]$EndUtc = [DateTime]::MaxValue) {
    if ($EndUtc -ne [DateTime]::MaxValue) { $EndUtc = $EndUtc.ToUniversalTime() }
    $samples = [Collections.Generic.List[object]]::new()
    if (Test-Path -LiteralPath $Path) {
        foreach ($line in [IO.File]::ReadLines($Path)) {
            $entry = $line | ConvertFrom-Json
            if ([DateTime]::Parse($entry.utc).ToUniversalTime() -ge $EndUtc) { continue }
            if (@($entry.gpu).Count -eq 0) { continue }
            $columns = ([string]@($entry.gpu)[0]).Split(',')
            if ($columns.Count -ne 9) { continue }
            try {
                $samples.Add([pscustomobject]@{ Time = [DateTime]::Parse($entry.utc).ToUniversalTime();
                    Temperature = [double]::Parse($columns[1].Trim(),[Globalization.CultureInfo]::InvariantCulture);
                    Power = [double]::Parse($columns[2].Trim(),[Globalization.CultureInfo]::InvariantCulture);
                    Clock = [double]::Parse($columns[4].Trim(),[Globalization.CultureInfo]::InvariantCulture);
                    MemoryMiB = [double]::Parse($columns[6].Trim(),[Globalization.CultureInfo]::InvariantCulture) })
            } catch { continue }
        }
    }
    $result = [ordered]@{ status = 'INSUFFICIENT_DATA'; validSamples = $samples.Count; windowSeconds = $WindowSeconds;
        thresholds = @{ temperatureMeanDeltaC = 2.0; temperatureSlopeCPerMinute = 0.5; graphicsClockRelativeDrift = 0.05; powerRelativeDrift = 0.1 };
        sampledDeviceMemoryPeakMiB = if ($samples.Count) { ($samples | Measure-Object MemoryMiB -Maximum).Maximum } else { $null };
        endUtcExclusive = $EndUtc.ToString('o');
        interpretation = 'Two adjacent end windows must pass every test. The optional exclusive frame-end boundary excludes teardown. Device-wide telemetry is a thermal proxy; sampled peaks can miss short spikes.' }
    if ($samples.Count -lt 2 -or ($samples[-1].Time - $samples[0].Time).TotalSeconds -lt 2 * $WindowSeconds) { return $result }
    $end = $samples[-1].Time
    $first = @($samples | Where-Object { $_.Time -gt $end.AddSeconds(-2*$WindowSeconds) -and $_.Time -le $end.AddSeconds(-$WindowSeconds) })
    $second = @($samples | Where-Object { $_.Time -gt $end.AddSeconds(-$WindowSeconds) })
    if ($first.Count -lt $WindowSeconds/2 -or $second.Count -lt $WindowSeconds/2) { return $result }
    $firstTemperature = ($first | Measure-Object Temperature -Average).Average
    $secondTemperature = ($second | Measure-Object Temperature -Average).Average
    $firstClock = ($first | Measure-Object Clock -Average).Average
    $secondClock = ($second | Measure-Object Clock -Average).Average
    $firstPower = ($first | Measure-Object Power -Average).Average
    $secondPower = ($second | Measure-Object Power -Average).Average
    $xMean = ($second | ForEach-Object { ($_.Time-$end).TotalMinutes } | Measure-Object -Average).Average
    $covariance = 0.0; $variance = 0.0
    foreach ($sample in $second) {
        $x = ($sample.Time-$end).TotalMinutes-$xMean
        $covariance += $x * ($sample.Temperature-$secondTemperature)
        $variance += $x*$x
    }
    $result.temperatureMeanDeltaC = [Math]::Abs($secondTemperature-$firstTemperature)
    $result.temperatureSlopeCPerMinute = if ($variance -gt 0) { $covariance/$variance } else { [double]::PositiveInfinity }
    $result.graphicsClockRelativeDrift = [Math]::Abs($secondClock-$firstClock)/[Math]::Max(1.0,$firstClock)
    $result.powerRelativeDrift = [Math]::Abs($secondPower-$firstPower)/[Math]::Max(1.0,$firstPower)
    $result.status = if ($result.temperatureMeanDeltaC -le 2 -and [Math]::Abs($result.temperatureSlopeCPerMinute) -le 0.5 -and
        $result.graphicsClockRelativeDrift -le 0.05 -and $result.powerRelativeDrift -le 0.1) { 'PASS_TELEMETRY_PROXY' } else { 'FAIL_UNSTABLE_TELEMETRY' }
    return $result
}
