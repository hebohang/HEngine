[CmdletBinding()]
param(
    [string]$Project = '',
    [Parameter(Mandatory = $true)]
    [string]$StartupScene,
    [ValidateSet("Forward", "Deferred")]
    [string]$RenderPath = "Deferred",
    [ValidateSet("DX12", "Vulkan")]
    [string]$GraphicsApi = "DX12",
    [ValidateRange(0, 1000000)]
    [int]$WarmupFrames = 36000,
    [ValidateRange(1, 1000000)]
    [int]$MeasureFrames = 18000,
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 1800,
    [string]$Resolution = "3840x2160",
    [string]$OutputDirectory = "",
    [string]$PowerModeNote = "not recorded",
    [Parameter(Mandatory = $true)]
    [string]$RenderSettingsPath,
    [Parameter(Mandatory = $true)]
    [string]$BenchmarkInputPath,
    [ValidateRange(15, 900)]
    [int]$ThermalWindowSeconds = 120,
    [switch]$PrepareOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Process.ps1")
. (Join-Path $PSScriptRoot "Rendering-BenchmarkAnalysis.ps1")
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot 'Project.ps1')
if (!$Project) { $Project = Join-Path $repositoryRoot 'Examples/RenderingTests' }
$projectInfo = Resolve-HEProject $Project $repositoryRoot
$projectRoot = $projectInfo.Root
$gameBuild = if ($projectRoot -ieq (Join-Path $repositoryRoot 'Project')) { 'out/agent/game-relwithdebinfo' } else { "out/agent/projects/$($projectInfo.CacheKey)/game-relwithdebinfo" }
$expectedArtifact = Join-Path $repositoryRoot "$gameBuild/bin/$($projectInfo.ExecutableName).exe"
if ($StartupScene -notmatch '^Assets/[^"\r\n]+\.he$' -or
    @($StartupScene.Split('/')) -contains '..') {
    throw "StartupScene requires a safe Assets-relative .he path."
}
if ($Resolution -notmatch '^(?<width>[1-9][0-9]{0,3})x(?<height>[1-9][0-9]{0,3})$') {
    throw "Resolution must use WIDTHxHEIGHT."
}
$expectedWidth = [int]$Matches.width
$expectedHeight = [int]$Matches.height
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ("out\agent\benchmarks\{0}-{1}-{2}" -f
        (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $RenderPath.ToLowerInvariant(), $PID)
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Refusing to reuse an existing benchmark directory: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project $projectRoot -Workflow 'benchmark-rendering' -Paths @($OutputDirectory) -Outer
$storageState = 'failed'
$process = $null
try {

$framesPath = Join-Path $OutputDirectory "frames.csv"
$stdoutPath = Join-Path $OutputDirectory "runner.stdout.log"
$stderrPath = Join-Path $OutputDirectory "runner.stderr.log"
$telemetryPath = Join-Path $OutputDirectory "telemetry.jsonl"
$configurationPath = Join-Path $OutputDirectory "configuration.json"
$resultPath = Join-Path $OutputDirectory "result.json"
$scenePath = Join-Path $projectRoot $StartupScene
$benchmarkInput = Get-Content -Raw -LiteralPath $BenchmarkInputPath | ConvertFrom-Json
if ($benchmarkInput.scene -ne $StartupScene -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $scenePath).Hash -ne $benchmarkInput.sceneSha256) {
    throw "Benchmark scene differs from its authored, hashed input manifest."
}
$trajectoryPath = Join-Path $projectRoot $benchmarkInput.trajectory
$scriptPath = Join-Path $projectRoot $benchmarkInput.script
foreach ($assetCheck in @(@($trajectoryPath, $benchmarkInput.trajectorySha256), @($scriptPath, $benchmarkInput.scriptSha256))) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $assetCheck[0]).Hash -ne $assetCheck[1]) {
        throw "Benchmark trajectory or camera script changed after authoring."
    }
}
if ($benchmarkInput.randomSeed -ne 'not_applicable') { throw "This benchmark requires an explicitly deterministic static scene without random components." }
$trajectory = Get-Content -Raw -LiteralPath $trajectoryPath | ConvertFrom-Json
if ($trajectory.geometryClearance.status -ne 'PASS') { throw "Camera path lacks successful geometry clearance validation." }
$fixedDelta = [double]$benchmarkInput.fixedDeltaSeconds
if ($WarmupFrames % [int]$benchmarkInput.periodFrames -ne 0 -or $MeasureFrames % [int]$benchmarkInput.periodFrames -ne 0) {
    throw "Warmup and measured frame counts must both contain whole authored camera cycles."
}
if ($benchmarkInput.environment) {
    $environmentPath = Join-Path $projectRoot $benchmarkInput.environment.path
    if ((Get-FileHash -LiteralPath $environmentPath -Algorithm SHA256).Hash -ne $benchmarkInput.environment.sha256) {
        throw 'Benchmark environment hash mismatch.'
    }
}
$qualityInput = Get-Content -Raw -LiteralPath $RenderSettingsPath | ConvertFrom-Json
$requiredQuality = @('Rendering.AO.Enabled','Rendering.AO.Radius','Rendering.AO.Intensity',
    'Rendering.AO.SliceCount','Rendering.AO.StepsPerSide','Rendering.Reflections.SSR.GuideCacheEnabled',
    'Rendering.GpuBatching.Enabled','Rendering.GpuBatching.ShadowsEnabled','Rendering.GI.SVGF.OptimizedKernels',
    'Rendering.AntiAliasing.TaaOptimizedHistoryGather',
    'Rendering.Memory.TransientTextureReuse',
    'Rendering.Reflections.SSR.Enabled','Rendering.Reflections.SSR.StepCount','Rendering.Reflections.SSR.MaximumDistance',
    'Rendering.Reflections.Probes.Enabled','Rendering.GI.Technique','Rendering.GI.RenderScale','Rendering.GI.SSGI.RayCount',
    'Rendering.GI.SSGI.StepCount','Rendering.AntiAliasing.Mode','Rendering.AntiAliasing.TaaHistoryWeight',
    'Rendering.LOD.Enabled','Rendering.LOD.MaxPixelError','Rendering.Occlusion.Enabled',
    'Rendering.PostProcess.Bloom.Intensity','Rendering.PostProcess.AutoExposure.Enabled','Rendering.Shadows.Enabled')
foreach ($id in $requiredQuality) {
    if ($null -eq $qualityInput.PSObject.Properties[$id]) { throw "Quality configuration must explicitly declare $id" }
}
$qualityPath = Join-Path $OutputDirectory 'render-settings.input.json'
Copy-Item -LiteralPath $RenderSettingsPath -Destination $qualityPath
$settingsSnapshotPath = $framesPath + '.settings.json'
$passesPath = $framesPath + '.passes.csv'
$nvidiaSmi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
$sourceState = @(& git -C $repositoryRoot status --short)
$powerScheme = @(& powercfg /getactivescheme)
$cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
$computer = Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model, TotalPhysicalMemory
$battery = @(Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue |
    Select-Object PowerOnline, Discharging, Charging)
$gpu = if ($nvidiaSmi) {
    @(& $nvidiaSmi.Source --query-gpu=name,uuid,driver_version,memory.total --format=csv,noheader,nounits)
} else { @() }
$metadata = [ordered]@{
    schemaVersion = 2
    createdUtc = [DateTime]::UtcNow.ToString("o")
    sourceCommit = (& git -C $repositoryRoot rev-parse HEAD)
    sourceState = $sourceState
    scene = $StartupScene
    sceneSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $scenePath).Hash
    renderPath = $RenderPath
    graphicsApi = $GraphicsApi
    requestedWidth = $expectedWidth
    requestedHeight = $expectedHeight
    warmupFrames = $WarmupFrames
    measureFrames = $MeasureFrames
    fixedSimulationDeltaSeconds = $fixedDelta
    vsync = $false
    frameLimiter = "none requested"
    benchmarkInput = $benchmarkInput
    cameraTrajectory = $trajectory
    randomSeed = 'not_applicable'
    randomSeedInterpretation = 'No random scene components; fixed-tick authored motion. Engine temporal sampling is frame-index driven, not user-seeded.'
    qualityInput = $qualityInput
    qualityInputSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $qualityPath).Hash
    auxiliaryWork = @{ GTAO = ('ceil(native/2), {0} slices x {1} steps x 2 sides, fixed spatial rotation, bilateral reconstruction' -f $qualityInput.'Rendering.AO.SliceCount', $qualityInput.'Rendering.AO.StepsPerSide'); SSR = 'ceil(native/2), configured step/refinement count, native reconstruction, main TAA stabilization'; Bloom = 'ceil(native/4), two nine-tap filters'; GI = 'Rendering.GI.RenderScale and SSGI samples/steps in explicit input; updated each frame'; MainView = 'Native requested dimensions; no upscale/dynamic resolution/frame generation/VRS' }
    powerModeNote = $PowerModeNote
    activePowerScheme = $powerScheme
    battery = $battery
    cpu = $cpu
    computer = $computer
    gpu = $gpu
    telemetryIntervalSeconds = 1
    telemetryInterpretation = "nvidia-smi is device-wide and includes other processes; process working set is sampled, not allocation peak."
    cpuInterpretation = "Steady-clock application frame wall time includes render submission, present and GPU backpressure; not pure CPU compute time."
    gpuInterpretation = "gpuFrameSpanMs brackets the complete RenderGraph; gpuPassSumMs separately sums passes. Both are delayed and keyed by gpuSample. GPU analysis deduplicates IDs and excludes sample IDs through first measured GPU ID plus frames-in-flight to conservatively remove warmup. Present and external work are outside this graph interval."
    thermalInterpretation = "Whole camera-cycle warmup; compare the final two telemetry windows for temperature slope/mean and clock/power drift. This is a recorded stability proxy, not GPU utilization as efficiency evidence."
    thermalWindowSeconds = $ThermalWindowSeconds
}
$metadata | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $configurationPath -Encoding UTF8

$arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
    (Join-Path $PSScriptRoot "hectl.ps1"), "game", "check",
    "-Project", $projectRoot,
    "-Configuration", "RelWithDebInfo", "-TimeoutSeconds", "$TimeoutSeconds",
    "-Frames", "$($WarmupFrames + $MeasureFrames)", "-Resolution", $Resolution,
    "-StartupScene", $StartupScene, "-RenderPath", $RenderPath,
    "-GraphicsApi", $GraphicsApi, "-StrictGraphicsApi", "-RenderStats",
    "-RenderStatsPath", $framesPath, "-PerformanceWarmupFrames", "$WarmupFrames")
$arguments += @('-FixedDeltaSeconds', $fixedDelta.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
    '-RenderSettingsPath', $qualityPath)
$quotedArguments = foreach ($argument in $arguments) {
    if ($argument.Contains('"') -or $argument.Contains("`r") -or $argument.Contains("`n")) {
        throw "A benchmark argument contains invalid quoting characters."
    }
    '"' + $argument + '"'
}
$metadata.command = "powershell.exe " + ($quotedArguments -join ' ')
$metadata | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $configurationPath -Encoding UTF8
if ($PrepareOnly) {
    $storageState = 'success'
    Write-Host "Benchmark inputs validated and exact command recorded; no build or HGame run: $OutputDirectory"
    return
}
$previousBenchmarkOutput = [Environment]::GetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT','Process')
[Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$OutputDirectory,'Process')
$process = $null
$job = New-AgentProcessJob
$gameProcessId = 0
$peakWorkingSetBytes = 0L
$peakPrivateBytes = 0L
$runClock = [Diagnostics.Stopwatch]::StartNew()
try {
    $startParameters = @{
        FilePath = "powershell.exe"
        ArgumentList = $quotedArguments
        PassThru = $true
        WindowStyle = "Hidden"
        WorkingDirectory = $repositoryRoot
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
    }
    $process = Start-Process @startParameters
    Add-AgentProcessToJob -Job $job -Process $process
    while (-not $process.HasExited) {
        if ($runClock.Elapsed.TotalSeconds -gt ($TimeoutSeconds + 900)) {
            throw "Benchmark build/run wrapper exceeded its total deadline."
        }
        if ($gameProcessId -eq 0 -and (Test-Path -LiteralPath $stdoutPath)) {
            $started = Select-String -LiteralPath $stdoutPath -Pattern 'run HGame: STARTED pid=(\d+)' |
                Select-Object -Last 1
            if ($started) { $gameProcessId = [int]$started.Matches[0].Groups[1].Value }
        }
        $game = if ($gameProcessId -gt 0) {
            Get-Process -Id $gameProcessId -ErrorAction SilentlyContinue
        } else { $null }
        $observedPath = $null
        $observedStartUtc = $null
        if ($game) {
            # Windows may expose the new PID before its main-module path is
            # queryable (or the process can exit during this observation).
            # Retry on the next poll; never sample an unverified process.
            try {
                $observedPath = $game.Path
                $observedStartUtc = $game.StartTime.ToUniversalTime().ToString('o')
            } catch { $game = $null }
            if ([string]::IsNullOrWhiteSpace($observedPath)) { $game = $null }
        }
        if ($game) {
            $artifactPath = [IO.Path]::GetFullPath($observedPath)
            if (-not [string]::Equals($artifactPath, $expectedArtifact, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Observed benchmark process uses an unexpected executable: $artifactPath"
            }
            if ($metadata.Contains('executedArtifact') -and $metadata.executedArtifact.processStartUtc -ne $observedStartUtc) {
                throw 'Benchmark PID was reused by a different process instance.'
            }
            if (-not $metadata.Contains('executedArtifact')) {
                $metadata.executedArtifact = @{
                    path = $artifactPath
                    sha256 = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash
                    lastWriteUtc = (Get-Item -LiteralPath $artifactPath).LastWriteTimeUtc.ToString('o')
                    processStartUtc = $observedStartUtc
                }
                $metadata | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $configurationPath -Encoding UTF8
            }
            $peakWorkingSetBytes = [Math]::Max($peakWorkingSetBytes, $game.PeakWorkingSet64)
            $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $game.PrivateMemorySize64)
            $sample = [ordered]@{
                utc = [DateTime]::UtcNow.ToString("o")
                processId = $gameProcessId
                workingSetBytes = $game.WorkingSet64
                peakWorkingSetBytes = $game.PeakWorkingSet64
                privateBytes = $game.PrivateMemorySize64
                gpu = if ($nvidiaSmi) {
                    @(& $nvidiaSmi.Source --query-gpu=timestamp,temperature.gpu,power.draw,power.limit,clocks.current.graphics,clocks.current.memory,memory.used,utilization.gpu,pstate --format=csv,noheader,nounits)
                } else { @() }
                gpuColumns = "timestamp,temperatureC,powerW,powerLimitW,graphicsMHz,memoryMHz,deviceMemoryMiB,utilizationPercent,pstate"
            }
            $sample | ConvertTo-Json -Depth 4 -Compress |
                Add-Content -LiteralPath $telemetryPath -Encoding UTF8
        }
        Start-Sleep -Milliseconds 1000
        $process.Refresh()
    }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Benchmark HGame check failed (exit=$($process.ExitCode)); see runner logs."
    }
}
finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-AgentOwnedProcess -Process $process -Description "render benchmark"
        }
    }
    Close-AgentProcessJob -Job $job
    [Environment]::SetEnvironmentVariable('HE_RENDER_BENCHMARK_OUTPUT',$previousBenchmarkOutput,'Process')
}

if (-not $metadata.Contains('executedArtifact')) { throw 'Benchmark finished without a verified executable/process identity.' }
$settings = Get-Content -Raw -LiteralPath $settingsSnapshotPath | ConvertFrom-Json
if ($settings.renderer.phase -ne 'shutdown' -or $settings.renderer.width -ne $expectedWidth -or $settings.renderer.height -ne $expectedHeight) {
    throw 'Actual renderer settings/dimensions lack a completed shutdown snapshot.'
}
if ($settings.renderer.framesInFlight -ne $settings.effective.'Rendering.Presentation.FramesInFlight') {
    throw 'Actual backend frame count differs from effective settings.'
}
$expectedQuality = @{}
foreach ($property in $qualityInput.PSObject.Properties) { $expectedQuality[$property.Name] = $property.Value }
$expectedQuality['Rendering.Path'] = $RenderPath
$expectedQuality['Rendering.Backend'] = $GraphicsApi
$expectedQuality['Rendering.Presentation.VSync'] = $false
foreach ($id in $expectedQuality.Keys) {
    $effective = $settings.effective.PSObject.Properties[$id]
    $expected = $expectedQuality[$id]
    $matches = $null -ne $effective
    if ($matches -and ($expected -is [bool] -or $expected -is [string])) {
        $matches = $effective.Value -eq $expected
    } elseif ($matches) {
        $matches = [Math]::Abs([double]$effective.Value - [double]$expected) -le [Math]::Max(0.000001,[Math]::Abs([double]$expected)*0.00001)
    }
    if (-not $matches) {
        throw "Requested quality differs from actual effective setting: $id. See the settings snapshot; no silent fallback is accepted."
    }
}
$cameraEvidence = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'camera.runtime.json') | ConvertFrom-Json
if ($cameraEvidence.status -ne 'PASS_FIXED_TICK_INPUT' -or $cameraEvidence.trajectorySha256 -ne $benchmarkInput.trajectorySha256 -or
    $cameraEvidence.scriptSha256 -ne $benchmarkInput.scriptSha256 -or $cameraEvidence.ticks -ne ($WarmupFrames + $MeasureFrames)) {
    throw 'Runtime camera input did not complete the verified fixed-tick trajectory.'
}

$rows = @(Import-Csv -LiteralPath $framesPath)
if ($rows.Count -ne $MeasureFrames) {
    throw "Measured frame count differs from request: actual=$($rows.Count), expected=$MeasureFrames."
}
if (@($rows | Where-Object { [int]$_.width -ne $expectedWidth -or [int]$_.height -ne $expectedHeight }).Count -gt 0) {
    throw "Actual render dimensions changed or differ from requested native resolution."
}
$culture = [Globalization.CultureInfo]::InvariantCulture
$times = @($rows | ForEach-Object { [double]::Parse($_.cpuWallFrameMs, $culture) })
$sortedTimes = @($times | Sort-Object)
function Get-Percentile([double]$p, [double[]]$Samples = $sortedTimes) {
    if ($Samples.Count -eq 0) { return $null }
    $position = ($Samples.Count - 1) * $p
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    $Samples[$lower] + ($Samples[$upper] - $Samples[$lower]) * ($position - $lower)
}
$gpuCutoff = [uint64]$rows[0].gpuWarmupCutoffSample
$gpuRows = @($rows | Where-Object {
    [uint64]$_.gpuSample -gt $gpuCutoff -and [double]::Parse($_.gpuFrameSpanMs, $culture) -gt 0.0
} | Group-Object -Property gpuSample | ForEach-Object { $_.Group[0] })
$gpuTimes = @($gpuRows | ForEach-Object { [double]::Parse($_.gpuFrameSpanMs, $culture) })
$sortedGpuTimes = @($gpuTimes | Sort-Object)
$uploadBytes = @($rows | ForEach-Object { [uint64]$_.sceneTableUploadedBytes })
$cpuPhaseStatistics = [ordered]@{}
foreach ($column in @("updateMs", "renderSubmitPresentMs", "windowMs", "cpuAcquireImageMs",
    "cpuFrameSlotWaitMs", "cpuGraphicsSubmitMs", "cpuPresentMs", "cpuGarbageCollectMs",
    "cpuCommandListSetupMs", "cpuRenderOtherMs")) {
    $phaseSamples = @($rows | ForEach-Object { [double]::Parse($_.$column, $culture) })
    $sortedPhaseSamples = @($phaseSamples | Sort-Object)
    $cpuPhaseStatistics[$column] = [ordered]@{
        averageMs = ($phaseSamples | Measure-Object -Average).Average
        p95Ms = (Get-Percentile 0.95 -Samples $sortedPhaseSamples)
        p99Ms = (Get-Percentile 0.99 -Samples $sortedPhaseSamples)
        maxMs = (Get-Percentile 1.0 -Samples $sortedPhaseSamples)
    }
}
$invalidCpuPartitions = @($rows | Where-Object {
    $sum = 0.0
    foreach ($column in @("cpuAcquireImageMs", "cpuFrameSlotWaitMs", "cpuGraphicsSubmitMs",
        "cpuPresentMs", "cpuGarbageCollectMs", "cpuCommandListSetupMs", "cpuRenderOtherMs")) {
        $sum += [double]::Parse($_.$column, $culture)
    }
    [Math]::Abs($sum - [double]::Parse($_.renderSubmitPresentMs, $culture)) -gt 0.01
})
if ($invalidCpuPartitions.Count -gt 0) {
    throw "Disjoint backend CPU phases do not partition the measured rendering interval."
}
$overBudget60 = @($times | Where-Object { $_ -gt (1000.0 / 60.0) }).Count
$longest = 0
$consecutive = 0
foreach ($time in $times) {
    if ($time -gt (1000.0 / 60.0)) { $consecutive++; $longest = [Math]::Max($longest, $consecutive) }
    else { $consecutive = 0 }
}
$mean = ($times | Measure-Object -Average).Average
$passStatistics = Get-RenderPassStatistics -Path $passesPath -SampleIds @($gpuRows | ForEach-Object { [uint64]$_.gpuSample })
if ($passStatistics.Count -eq 0) { throw 'No completed measured GPU pass samples were exported.' }
$runLogLine = Select-String -LiteralPath $stdoutPath -Pattern 'stdout: (.+); stderr:' | Select-Object -Last 1
if (!$runLogLine) { throw 'No owned runtime log path for thermal frame-end verification.' }
$runtimeLogPath = $runLogLine.Matches[0].Groups[1].Value
$endLine = Select-String -LiteralPath $runtimeLogPath -Pattern '\[(\d\d:\d\d:\d\d)\].*Completed frame limit:' | Select-Object -Last 1
if (!$endLine) { throw 'No actual frame-limit timestamp for thermal verification.' }
$processStartLocal = [DateTime]::Parse($metadata.executedArtifact.processStartUtc).ToLocalTime()
$frameEndLocal = $processStartLocal.Date.Add([TimeSpan]::Parse($endLine.Matches[0].Groups[1].Value))
if ($frameEndLocal -lt $processStartLocal) { $frameEndLocal = $frameEndLocal.AddDays(1) }
# Logger timestamps have second precision. Exclude the entire completion
# second conservatively, as well as subsequent graphics teardown samples.
$thermal = Get-RenderThermalStability -Path $telemetryPath -WindowSeconds $ThermalWindowSeconds -EndUtc $frameEndLocal.ToUniversalTime()
$workloadStability = Get-RenderWorkloadStability -Rows $rows -PeriodFrames ([int]$benchmarkInput.periodFrames)
$resourceStatistics = [ordered]@{}
foreach ($column in @('gpuDrivenObjects','gpuVisibleObjects','gpuBatches','shadowDraws','transientReservedBytes',
    'gpuVisibleTriangles','baseRasterTriangles','selectedRasterTriangles','reducedLodSections',
    'geometryArenaReservedBytes','geometryArenaUsedBytes','indirectCompactionResidentBytes',
    'indirectCompactionUploadedBytes','gpuMultiDrawGroups')) {
    if ($null -ne $rows[0].PSObject.Properties[$column]) {
        $resourceStatistics[$column] = Get-RenderSampleStatistics -Samples @($rows | ForEach-Object { [double]::Parse($_.$column,$culture) })
    } else { $resourceStatistics[$column] = @{ status = 'UNAVAILABLE_IN_RUNTIME_CSV' } }
}
$result = [ordered]@{
    evidenceType = "runtime-manual"
    command = $metadata.command
    nativeResolutionValidation = "PASS"
    effectiveQualityValidation = 'PASS_REQUESTED_EQUALS_ACTUAL'
    settingsSnapshot = $settingsSnapshotPath
    deterministicCameraInput = $cameraEvidence
    thermalStability = $thermal
    workloadStability = $workloadStability
    gpuPassStatisticsMilliseconds = $passStatistics
    gpuPassInterpretation = 'Every pass sample is from the same completed GPU sample IDs used for graph-span statistics; no summary is relabeled as per-frame timing. Passes and CPU/GPU intervals must not be added as elapsed frame time.'
    resourceStatistics = $resourceStatistics
    processMemoryBudget = Get-RenderMemoryBudgetStatistics -Path ($framesPath + '.memory.csv') -FirstFrame ([long]$rows[0].frame) -LastFrame ([long]$rows[-1].frame)
    resourceInterpretation = 'GPU visible objects/triangles belong to delayed GPU opaque visibility. Base/selected raster triangles and reduced LOD sections are CPU extraction totals before GPU culling. gpuBatches counts candidate batches; gpuMultiDrawGroups counts actual GPU main-path submission groups. Geometry arena reserved/used bytes and indirect-compaction resident/uploaded bytes are distinct from texture payload and driver VRAM. Shadow counts/transient reservation remain separate.'
    samples = $rows.Count
    cpuWallAverageMs = $mean
    cpuPhaseStatistics = $cpuPhaseStatistics
    cpuPhaseInterpretation = "FrameSlotWait measures the actual slot-completion query wait; AcquireImage and Present may include OS/driver pacing. Submit includes driver submission and event-query arming. RenderOther is the disjoint remainder including scene preparation, command recording and CPU UI; it is not isolated CPU busy time. These CPU intervals must not be added to the GPU span."
    averageFps = 1000.0 / $mean
    p95Ms = (Get-Percentile 0.95)
    p99Ms = (Get-Percentile 0.99)
    maxMs = (Get-Percentile 1.0)
    overBudget60 = $overBudget60
    overBudget90 = @($times | Where-Object { $_ -gt (1000.0 / 90.0) }).Count
    longestConsecutiveOverBudget60 = $longest
    framesOver50Milliseconds = @($times | Where-Object { $_ -gt 50 }).Count
    framesOver100Milliseconds = @($times | Where-Object { $_ -gt 100 }).Count
    gpuFrameSpanSamples = $gpuTimes.Count
    gpuWarmupCutoffSample = $gpuCutoff
    gpuFrameSpanAverageMs = ($gpuTimes | Measure-Object -Average).Average
    gpuFrameSpanP95Ms = (Get-Percentile 0.95 -Samples $sortedGpuTimes)
    gpuFrameSpanP99Ms = (Get-Percentile 0.99 -Samples $sortedGpuTimes)
    gpuFrameSpanMaxMs = (Get-Percentile 1.0 -Samples $sortedGpuTimes)
    gpuFrameSpanOverBudget60 = @($gpuTimes | Where-Object { $_ -gt (1000.0 / 60.0) }).Count
    sceneTableUploadBytes = ($uploadBytes | Measure-Object -Sum).Sum
    averageSceneTableUploadBytes = ($uploadBytes | Measure-Object -Average).Average
    peakSceneTableUploadBytes = ($uploadBytes | Measure-Object -Maximum).Maximum
    measuredSeconds = ($times | Measure-Object -Sum).Sum / 1000.0
    peakWorkingSetBytes = $peakWorkingSetBytes
    peakSampledPrivateBytes = $peakPrivateBytes
    strict60FrameBudget = if ($overBudget60 -eq 0) { "PASS" } else { "FAIL" }
    acceptance = "NOT_EVALUATED: image quality and motion stability require separate visual evidence. Strict frame-budget, actual configuration, fixed trajectory and thermal proxy results are reported individually. GPU graph span excludes present/external work."
}
$result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host ("Benchmark captured: frames={0}; P95={1:n3} ms; P99={2:n3} ms; over16.667={3}; artifacts: {4}" -f
    $rows.Count, $result.p95Ms, $result.p99Ms, $overBudget60, $OutputDirectory)

$storageState = 'success'
}
finally {
    if ($process) { try { $process.Refresh(); if (-not $process.HasExited) { $storageState = 'unknown' } } catch { $storageState = 'unknown' } }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
