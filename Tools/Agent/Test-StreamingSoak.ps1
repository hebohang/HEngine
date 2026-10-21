[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [string]$StartupScene='Assets/Scenes/Diagnostics/StreamingTraversal.he',
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
 [ValidateRange(600,3600)][int]$PhaseFrames=3600,
 # Optional read-only hardware sampling for attributing GPU-wide slowdowns.
 [switch]$NvidiaTelemetry,
 [switch]$CpuTelemetry
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Could not resolve project'}
$context=($contextJson -join "`n")|ConvertFrom-Json
$exe=Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe')
$session=Join-Path $PSScriptRoot 'Session.ps1'
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;ArtifactPath=$exe;
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=300;
 ExistingBuildSha256=$ExistingBuildSha256}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'streaming-soak' -Paths @($OutputDirectory) -Outer
$state='failed';$started=$false;$owner=$null;$phases=@();$samples=0;$captured=$false
$result=@{status='FAILED';api=$GraphicsApi;shutdownVerified=$false}
$gpuMonitor=$null
$cpuMonitor=$null
$gpuFields='timestamp,index,uuid,pstate,clocks.current.graphics,clocks.current.memory,utilization.gpu,utilization.memory,power.draw,temperature.gpu,clocks_throttle_reasons.active'
$previousBudget=$env:HE_TEXTURE_STREAMING_BUDGET_MB
Remove-Item Env:HE_TEXTURE_STREAMING_BUDGET_MB -ErrorAction SilentlyContinue
function Call([string]$method,[hashtable]$parameters=@{}) {
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($parameters|ConvertTo-Json -Depth 12 -Compress))
 $result=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$result.ok){throw ($result|ConvertTo-Json -Depth 12 -Compress)}
 return $result.result
}

function Settings([hashtable]$patch){
 $s=Call 'render.settings.get'
 $r=Call 'render.settings.apply' @{baseRevision=$s.revision;baseHash=$s.hash;patch=$patch}
 if(!$r.success){throw ($r|ConvertTo-Json -Depth 10 -Compress)}
}
function Capture([string]$name){
 $capture=Call 'capture.frame' @{filename="$name.png";delayFrames=2}
 Copy-Item $capture.path "$OutputDirectory/$name.png"
 Copy-Item ($capture.path+'.world.json') "$OutputDirectory/$name.world.json"
 $w=Get-Content ($capture.path+'.world.json') -Raw|ConvertFrom-Json
 if(!$w.recordedRaster.upscaling.active -or $w.recordedRaster.upscaling.failure -or $w.recordedRaster.taaActive){throw 'Invalid FSR/TAA state'}
 if($w.recordedRaster.textureStreaming.failedUploads -ne 0){throw 'Streaming upload failed'}
 return $w
}
function Observe([string]$phase){
 if($gpuMonitor -and $gpuMonitor.HasExited){throw 'Owned NVIDIA telemetry process exited before capture completed'}
 if($cpuMonitor -and $cpuMonitor.HasExited){throw 'Owned CPU telemetry process exited before capture completed'}
 $runtime=Call 'session.status'
 $process=Get-Process -Id $owner.pid -ErrorAction Stop
 if([IO.Path]::GetFullPath($process.Path) -ine [IO.Path]::GetFullPath($exe) -or $process.StartTime.ToUniversalTime().ToString('o') -ne $script:processStart){throw 'Process identity changed'}
 $counterPaths=@('Dedicated Usage','Shared Usage','Total Committed')|ForEach-Object {"\GPU Process Memory(pid_$($owner.pid)_*)\$_"}
 $counter=Get-Counter -Counter $counterPaths -ErrorAction Stop
 $values=@($counter.CounterSamples|ForEach-Object {@{instance=$_.InstanceName;path=$_.Path;status=$_.Status;bytes=$_.CookedValue}})
 if(!$values.Count -or ($values|Where-Object {($_.status -ne 0 -and $_.status -ne 1) -or $_.bytes -lt 0})){throw 'GPU process counters unavailable/invalid'}
 @{utc=[DateTime]::UtcNow.ToString('o');phase=$phase;renderFrame=$runtime.renderFrame;
   completedRenderFrames=$runtime.completedRenderFrames;simulationTimeSeconds=$runtime.simulationTimeSeconds;
   processPrivateBytes=$process.PrivateMemorySize64;processWorkingSetBytes=$process.WorkingSet64;
   gpuCounters=$values}|ConvertTo-Json -Depth 8 -Compress|Add-Content "$OutputDirectory/telemetry.jsonl" -Encoding UTF8
 $script:samples++
 return $runtime
}
try {
 & $session @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -Resolution 1280x720 -StartupScene $StartupScene -RespectPresentationSettings -RenderStats -RenderStatsPath "$OutputDirectory/frames.csv" -PerformanceWarmupFrames 30 *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $processStart=(Get-Process -Id $owner.pid).StartTime.ToUniversalTime().ToString('o')
 Settings @{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Quality';'Rendering.GI.Technique'='Off';'Rendering.TextureStreaming.BudgetMiB'=512}
 if($NvidiaTelemetry){
  $monitorExe=(Get-Command nvidia-smi -ErrorAction Stop).Source
  $probe=& $monitorExe "--query-gpu=$gpuFields" --format=csv,noheader,nounits
  if($LASTEXITCODE -ne 0 -or !$probe){throw 'NVIDIA telemetry query unavailable'}
  $gpuMonitor=Start-Process -FilePath $monitorExe -ArgumentList @("--query-gpu=$gpuFields",'--format=csv,noheader,nounits','--loop-ms=500','-f',('"'+$OutputDirectory+'/gpu-telemetry.csv"')) -WindowStyle Hidden -PassThru
  $null=$gpuMonitor.Handle
 }
 if($CpuTelemetry){
  $monitor=Join-Path $PSScriptRoot 'Monitor-CpuTelemetry.ps1'
  $cpuMonitor=Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$monitor+'"'),'-OutputPath',('"'+$OutputDirectory+'/cpu-telemetry.jsonl"')) -WindowStyle Hidden -PassThru -RedirectStandardError "$OutputDirectory/cpu-monitor.stderr.log"
  $null=$cpuMonitor.Handle
 }
 $null=Call 'runtime.resume'
 foreach($phase in @(@{name='warmup';budget=512;frames=1200},@{name='baseline';budget=512;frames=$PhaseFrames},@{name='pressure';budget=128;frames=$PhaseFrames},@{name='recovery';budget=512;frames=$PhaseFrames})){
  Settings @{'Rendering.TextureStreaming.BudgetMiB'=$phase.budget}
  $begin=Observe $phase.name;$timer=[Diagnostics.Stopwatch]::StartNew();$last=$begin
  do {
   Start-Sleep -Milliseconds 3000
   $last=Observe $phase.name
   if($timer.Elapsed.TotalSeconds -gt 600){throw 'Phase exceeded bounded wall-clock deadline'}
  }while(($last.renderFrame-$begin.renderFrame) -lt $phase.frames)
  $world=Capture $phase.name
  $phases+=@{name=$phase.name;budgetMiB=$phase.budget;start=$begin;end=$last;elapsedSeconds=$timer.Elapsed.TotalSeconds;
    streaming=$world.recordedRaster.textureStreaming;captureFrame=$world.applicationSimulationFrame;camera=$world.cameraPosition}
  $phases|ConvertTo-Json -Depth 18|Set-Content "$OutputDirectory/phases.json"
  Write-Host "$GraphicsApi $($phase.name) captured after $($last.renderFrame-$begin.renderFrame) frames, $([Math]::Round($timer.Elapsed.TotalSeconds,1)) seconds"
 }
 $result=@{status='CAPTURED';shutdownVerified=$false;api=$GraphicsApi;pid=$owner.pid;exeSha256=$ExistingBuildSha256;samples=$samples;
   nvidiaTelemetry=[bool]$NvidiaTelemetry;cpuTelemetry=[bool]$CpuTelemetry;gpuTelemetryFields=$gpuFields;localUtcOffsetMinutes=[TimeZoneInfo]::Local.GetUtcOffset([DateTime]::Now).TotalMinutes;
   scene=$StartupScene;sceneSha256=(Get-FileHash (Join-Path $context.project.Root $StartupScene)).Hash;
   scope='Fixed-delta camera orbit; VSync requested by fixture; no debug validation; native render stats and WDDM per-process counters'}
 $captured=$true
}finally {
 try {
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   # An already-dead process cannot prove successful shutdown through Stop.
   if(!(Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet)){throw 'Owned process exit was not verified'}
   $result.shutdownVerified=$true
  }
  if($captured -and $result.shutdownVerified){$result.status='CAPTURED_AND_STOPPED'}
 }catch {
  $result.status='FAILED';$result.shutdownError=$_.Exception.Message
  throw
 }finally {
  try {
   $monitorStopErrors=@()
   foreach($monitor in @($gpuMonitor,$cpuMonitor)){
    if(!$monitor){continue}
    # Only this invocation's process handles are terminated. Keep both monitors
    # alive through Game shutdown, and attempt both cleanups if one fails.
    try {if(!$monitor.HasExited){$monitor.Kill();$monitor.WaitForExit()}}
    catch {if(!$monitor.HasExited){$monitorStopErrors+=@($_.Exception.Message)}}
    finally {$monitor.Dispose()}
   }
   if($monitorStopErrors.Count){$result.status='FAILED';$result.monitorStopErrors=$monitorStopErrors}
   $result|ConvertTo-Json -Depth 8|Set-Content "$OutputDirectory/result.json"
   if($owner){Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log";Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"}
   if($result.status -eq 'CAPTURED_AND_STOPPED'){$state='success'}
  }finally {
   $env:HE_TEXTURE_STREAMING_BUDGET_MB=$previousBudget
   Complete-HEStorageRun -Run $storage -State $state
  }
 }
}

if($state -ne 'success'){throw 'Streaming soak did not complete cleanly; inspect result.json'}
