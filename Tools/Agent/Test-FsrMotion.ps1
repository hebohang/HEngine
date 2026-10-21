[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [string]$Fixture='Assets/Diagnostics/FsrMotion/fixture.json',
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12'
)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $output|Out-Null
$session=Join-Path $PSScriptRoot 'Session.ps1'
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed.'}
$context=($contextJson -join "`n")|ConvertFrom-Json
$Project=$context.project.Root
$exe=Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe')
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$Project;ArtifactPath=$exe;SessionRoot=$context.agentSessions;
 OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=300;ExistingBuildSha256=$ExistingBuildSha256}
$fixturePath=Join-Path $Project $Fixture
$fixtureData=Get-Content $fixturePath -Raw|ConvertFrom-Json
if(!$fixtureData.scene -or $fixtureData.frames -ne 16 -or $fixtureData.warmupSteps -ne 120){throw 'Invalid motion fixture contract.'}
Copy-Item -LiteralPath $fixturePath -Destination "$output/fixture.json"
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun=Start-HEStorageRun -Project $Project -Workflow 'fsr-motion' -Paths @($output) -Outer
$state='failed'
$started=$false;$owner=$null;$receipts=@();$baselineBefore=$null;$baselineAfter=$null
function Call([string]$method,[hashtable]$p=@{}) {
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){throw ($r|ConvertTo-Json -Depth 12 -Compress)}
 return $r.result
}
function Actor {
 $all=Call 'render.animation.status'
 return $all.objects|Where-Object {$_.uuid -eq $fixtureData.actor}|Select-Object -First 1
}
function Signature($actor) {return "$($actor.currentPaletteHash):$($actor.clothSolvers[0].positionsHash)"}
function Settings([string]$quality){
 $s=Call 'render.settings.get'
 $r=Call 'render.settings.apply' @{baseRevision=$s.revision;baseHash=$s.hash;patch=@{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'=$quality;'Rendering.Upscaling.Sharpness'=0;'Rendering.GI.Technique'='Off';'Rendering.PostProcess.AutoExposure.Enabled'=$false;'Rendering.PostProcess.ExposureCompensationEV'=0}}
 if(!$r.success){throw 'Settings failed'}
}
try {
 & $session @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -StartupScene $fixtureData.scene *> "$output/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause'
 $stages=@(@{name='quality-display';quality='Quality';source='automatic';format='png'},
           @{name='quality-motion';quality='Quality';source='motion';format='raw'},
           @{name='quality-ids';quality='Quality';source='surface-id';format='raw'},
           @{name='native-display';quality='NativeAA';source='automatic';format='png'},
           @{name='native-ids';quality='NativeAA';source='surface-id';format='raw'})
 foreach($stage in $stages) {
  Settings $stage.quality
  $load=Call 'runtime.scene.load' @{path=$fixtureData.scene;forceReload=$true}
  $ready=$false
  for($attempt=0;$attempt -lt 30;$attempt++){
   $summary=Call 'world.summary'
   if($summary.sceneGeneration -gt $load.previousSceneGeneration){$ready=$true;break}
  }
  if(!$ready){throw 'Reload did not commit'}
  $null=Call 'runtime.pause';$null=Call 'runtime.step' @{frames=120}
  $before=Actor;$before|ConvertTo-Json -Depth 24|Set-Content "$output/$($stage.name)-before.json"
  if(!$before -or !$before.clothSolvers.Count){throw 'Missing animated cloth actor'}
  $signature=Signature $before
  if($null -eq $baselineBefore){$baselineBefore=$signature}elseif($signature -ne $baselineBefore){throw "Replay initial pose differs: $signature vs $baselineBefore"}
  $sequence=Call 'capture.sequence' @{prefix=$stage.name;frames=16;format=$stage.format;source=$stage.source;cameraCut=$true;settleFrames=8}
  $receipts+=@{stage=$stage;sequence=$sequence;actorGpuIndex=$before.objectGpuIndex}
  foreach($frame in $sequence.frames){
   foreach($suffix in @('','.json','.world.json')){
    if(Test-Path ($frame.path+$suffix)){Copy-Item ($frame.path+$suffix) (Join-Path $output ((Split-Path -Leaf $frame.path)+$suffix))}
   }
  }
  if($sequence.sequenceHistoryArtifact.path){Copy-Item $sequence.sequenceHistoryArtifact.path "$output/$($stage.name)-history.json"}
  $after=Actor;$after|ConvertTo-Json -Depth 24|Set-Content "$output/$($stage.name)-after.json"
  $signature=Signature $after
  if($null -eq $baselineAfter){$baselineAfter=$signature}elseif($signature -ne $baselineAfter){throw "Replay final pose differs: $signature vs $baselineAfter"}
  # Collect output before writing the log; compact must not visit its own open log.
  $compression = & compact.exe /C /Q "$output/*" 2>&1
  if($LASTEXITCODE -ne 0){throw 'Evidence compression failed'}
  $compression | Set-Content "$output/compression.log"
  $nativeCaptures=Split-Path -Parent $sequence.frames[0].path
  $compression = & compact.exe /C /Q "$nativeCaptures/*" 2>&1
  if($LASTEXITCODE -ne 0){throw 'Native evidence compression failed'}
  $compression | Set-Content "$output/native-compression.log"
  Write-Host "$GraphicsApi $($stage.name) replay PASS"
 }
 $pause=Call 'capture.frame' @{filename='paused-motion.raw';source='motion';delayFrames=4}
 foreach($suffix in @('','.json','.world.json')){Copy-Item ($pause.path+$suffix) "$output/paused-motion.raw$suffix"}
 $scripts=Join-Path (Split-Path -Parent $exe) 'Scripting/CSharp/GameScripts.dll'
 @{api=$GraphicsApi;pid=$owner.pid;exeSha256=$common.ExistingBuildSha256;
   scriptsSha256=$(if(Test-Path $scripts){(Get-FileHash $scripts).Hash}else{$null});
   sceneSha256=(Get-FileHash (Join-Path $Project $fixtureData.scene)).Hash;
   fixtureSha256=(Get-FileHash $fixturePath).Hash;
   initialPose=$baselineBefore;finalPose=$baselineAfter;status='CAPTURED';frames=16}|ConvertTo-Json|Set-Content "$output/result.json"
 $state='success'
 Write-Host "Fast motion: $output"
}finally {
 try {
  $receipts|ConvertTo-Json -Depth 24|Set-Content "$output/receipts.json"
  if($started){& $session @common -Action Stop *> "$output/stop.log"}
  if($owner){Copy-Item "$($owner.runDirectory)/game.stdout.log" "$output/runtime.log";Copy-Item "$($owner.runDirectory)/game.stderr.log" "$output/stderr.log"}
 } finally {Complete-HEStorageRun -Run $storageRun -State $state}
}
