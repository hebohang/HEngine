$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Rendering-BenchmarkAnalysis.ps1')
$directory = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'out\agent\benchmarks\analysis-contract'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$statistics = Get-RenderSampleStatistics -Samples @(10,20,30,40)
if ((Get-RenderMemoryBudgetStatistics -Path (Join-Path $directory 'absent.csv') -FirstFrame 10 -LastFrame 20).status -ne 'UNAVAILABLE') {
    throw 'Missing memory budget was not kept unavailable.'
}
$budgetPath=Join-Path $directory 'memory.csv'
@('frame,available,processScope,localBudgetBytes,localUsageBytes','11,1,1,100,90') | Set-Content -LiteralPath $budgetPath
if ((Get-RenderMemoryBudgetStatistics -Path $budgetPath -FirstFrame 10 -LastFrame 20).status -ne 'UNAVAILABLE_PHASE_TAG') {
    throw 'Legacy memory CSV without startup phase was misclassified as measured-phase evidence.'
}
@('frame,available,processScope,localBudgetBytes,localUsageBytes,localReservationBytes,queryMilliseconds,nonLocalAvailable,nonLocalUsageBytes,automationFrame',
  '11,1,1,100,9999,0,0.01,1,10,0','1,1,1,100,500,0,0.01,1,10,1','10,0,0,0,0,0,0.01,0,0,1','11,1,1,100,90,0,0.02,1,10,1',
  '20,1,1,80,110,0,0.03,1,20,1','21,1,1,80,900,0,0.01,1,30,1') | Set-Content -LiteralPath $budgetPath
$budget=Get-RenderMemoryBudgetStatistics -Path $budgetPath -FirstFrame 10 -LastFrame 20
if ($budget.processBudgetObservations -ne 2 -or $budget.overBudgetObservations -ne 1 -or $budget.localUsageBytes.maximum -ne 110) {
    throw 'Memory budget warmup/range/unavailable filtering failed.'
}
if ($budget.allPhaseLocalUsagePeakBytes -ne 9999) { throw 'Loading memory peak was lost.' }
if ((Get-RenderMemoryBudgetStatistics -Path $budgetPath -FirstFrame 10 -LastFrame 10).status -ne 'NO_PROCESS_BUDGET_OBSERVATIONS') {
    throw 'Unsupported budget was treated as a valid zero.'
}
if ($statistics.p95 -ne 38.5 -or [Math]::Abs($statistics.p99-39.7) -gt 1e-9) { throw 'Type-7 percentiles failed.' }
$passesPath = Join-Path $directory 'passes.csv'
@('gpuSample,passName,milliseconds','1,Opaque,100','2,Opaque,2','3,Opaque,4','2,"Shadow,0",1','3,"Shadow,0",3') |
    Set-Content -LiteralPath $passesPath -Encoding ASCII
$passes = Get-RenderPassStatistics -Path $passesPath -SampleIds @(2,3)
if ($passes.Opaque.samples -ne 2 -or $passes.Opaque.average -ne 3 -or $passes['Shadow,0'].average -ne 2) { throw 'GPU sample alignment or quoted pass parsing failed.' }
$bridgeRows = @('gpuSample,passName,milliseconds,graphSubmissionId','1,Opaque,100,101','2,Opaque,2,103','3,Opaque,4,107','2,"Shadow,0",1,103','3,"Shadow,0",3,107')
$bridgeRows | Set-Content -LiteralPath $passesPath -Encoding ASCII
$passes = Get-RenderPassStatistics -Path $passesPath -SampleIds @(2,3)
if ($passes.Opaque.samples -ne 2 -or $passes.Opaque.average -ne 3 -or $passes['Shadow,0'].average -ne 2) { throw 'Submission-extended GPU schema changed sample statistics.' }
foreach ($badRow in @('2,Other,1,0','2,Other,1,109','4,Other,1,103','4,Other,1','4,Other,1,109,extra')) {
    @($bridgeRows + $badRow) | Set-Content -LiteralPath $passesPath -Encoding ASCII
    $rejected = $false
    try { $null = Get-RenderPassStatistics -Path $passesPath -SampleIds @(2,3,4) } catch { $rejected = $true }
    if (-not $rejected) { throw "Malformed/conflicting graph identity accepted: $badRow" }
}
$stablePath = Join-Path $directory 'stable.jsonl'
$unstablePath = Join-Path $directory 'unstable.jsonl'
$shortPath = Join-Path $directory 'short.jsonl'
$stable = @(); $unstable = @()
for ($second = 0; $second -le 60; $second++) {
    $time = [DateTime]::SpecifyKind([DateTime]'2026-09-08T00:00:00',[DateTimeKind]::Utc).AddSeconds($second)
    $stable += (@{ utc=$time.ToString('o'); gpu=@('time,65,80,95,1200,7000,2500,90,P0') } | ConvertTo-Json -Compress)
    $temperature = (65 + [Math]::Max(0,$second-30)*0.1).ToString('F2',[Globalization.CultureInfo]::InvariantCulture)
    $unstable += (@{ utc=$time.ToString('o'); gpu=@("time,$temperature,80,95,1200,7000,2500,90,P0") } | ConvertTo-Json -Compress)
}
$stable | Set-Content -LiteralPath $stablePath -Encoding UTF8
$unstable | Set-Content -LiteralPath $unstablePath -Encoding UTF8
$stable[0..4] | Set-Content -LiteralPath $shortPath -Encoding UTF8
$stableResult = Get-RenderThermalStability -Path $stablePath -WindowSeconds 15
$boundedResult = Get-RenderThermalStability -Path $stablePath -WindowSeconds 15 -EndUtc ([DateTime]'2026-09-08T00:00:31Z')
if ($boundedResult.validSamples -ne 31 -or $boundedResult.status -ne 'PASS_TELEMETRY_PROXY') { throw 'Thermal frame-end exclusion failed.' }
$unstableResult = Get-RenderThermalStability -Path $unstablePath -WindowSeconds 15
$shortResult = Get-RenderThermalStability -Path $shortPath -WindowSeconds 15
if ($stableResult.status -ne 'PASS_TELEMETRY_PROXY' -or $unstableResult.status -ne 'FAIL_UNSTABLE_TELEMETRY' -or
    $shortResult.status -ne 'INSUFFICIENT_DATA') { throw 'Thermal stable/unstable/insufficient-data classification failed.' }
$cycleRows = @(1..120 | ForEach-Object { [pscustomobject]@{ cpuWallFrameMs=18.5; gpuFrameSpanMs=18.4;
    gpuSample=$_; gpuWarmupCutoffSample=0; sceneTableUploadedBytes=0 } })
if ((Get-RenderWorkloadStability -Rows $cycleRows -PeriodFrames 60).status -ne 'PASS_REPEAT_CYCLE_PROXY') {
    throw 'Stable repeat-cycle classification failed.'
}
$cycleRows[5].cpuWallFrameMs=150; $cycleRows[5].gpuFrameSpanMs=145
if ((Get-RenderWorkloadStability -Rows $cycleRows -PeriodFrames 60).status -ne 'FAIL_NOT_SETTLED_OR_UNEXPLAINED_SPIKES' -or
    (Get-RenderWorkloadStability -Rows $cycleRows[0..59] -PeriodFrames 60).status -ne 'INSUFFICIENT_CAMERA_CYCLES') {
    throw 'Startup spike or insufficient repeat-cycle rejection failed.'
}
$report = @{ evidenceType='unit-test'; status='PASS'; command='powershell -NoProfile -File Tools/Agent/Test-RenderingBenchmarkAnalysis.ps1';
    checks=@('Type-7 P95/P99','GPU ID filtering and quoted pass names','Legacy and submission-extended pass schemas','Malformed and conflicting graph identity rejection','Memory budget phase/range/unsupported filtering','Stable telemetry','Rising-temperature rejection','Insufficient warmup rejection','Complete repeat-cycle settling and unexplained startup spikes');
    limitation='Synthetic analysis fixtures only, not measured HGame performance.' }
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directory 'validation.json') -Encoding UTF8
$report | ConvertTo-Json -Compress
