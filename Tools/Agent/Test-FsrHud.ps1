[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
 [string]$StartupScene='Assets/Scenes/SickaDynasty.he',
 [switch]$AllowNative4KRejection
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$json=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed'}
$context=($json -join "`n")|ConvertFrom-Json
$session=Join-Path $PSScriptRoot 'Session.ps1'
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;
 ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'));
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=240;ExistingBuildSha256=$ExistingBuildSha256}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'fsr-hud' -Paths @($OutputDirectory) -Outer
$started=$false;$finished=$false;$shutdownVerified=$false;$owner=$null;$state='failed';$failure=$null
$native4KRejected=$false;$receipts=@();$expectedSize=@(2560,1440)
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Call([string]$method,[hashtable]$p=@{}) {
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 Require $r.ok ($r|ConvertTo-Json -Depth 8 -Compress)
 return $r.result
}
function Settings([hashtable]$patch){
 $s=Call 'render.settings.get'
 $r=Call 'render.settings.apply' @{baseRevision=$s.revision;baseHash=$s.hash;patch=$patch}
 Require $r.success ($r|ConvertTo-Json -Depth 8 -Compress)
}
function Capture([string]$name,[bool]$active){
 $r=Call 'capture.frame' @{filename="$name.png";delayFrames=8}
 Copy-Item -LiteralPath $r.path -Destination "$OutputDirectory/$name.png"
 Copy-Item -LiteralPath ($r.path+'.world.json') -Destination "$OutputDirectory/$name.world.json"
 $world=Get-Content ($r.path+'.world.json') -Raw|ConvertFrom-Json
 $raster=$world.recordedRaster;$u=$raster.upscaling
 Require ($raster.valid -and $u.active -eq $active -and $raster.taaActive -ne $active) "$name FSR/TAA state mismatch"
 if($active){
  Require (!$u.failure -and $u.provider -eq '3.1.4' -and $u.quality -eq 'Performance') "$name FSR provider failed"
  for($i=0;$i -lt 2;++$i){Require ($u.outputSize[$i] -eq $expectedSize[$i] -and $u.renderSize[$i] -eq $expectedSize[$i]/2) "$name extent mismatch"}
 }else {
  Require ($raster.width -eq $expectedSize[0] -and $raster.height -eq $expectedSize[1]) "$name did not render at native resolution"
 }
 $elements=@{}
 foreach($id in @('objective','wave','crosshair','weapon','ammo','health','healthbar')){
  $elements[$id]=Call 'ui.element.rect' @{id=$id}
  Require $elements[$id].visible "Missing HUD $id"
 }
 $elements|ConvertTo-Json -Depth 10|Set-Content "$OutputDirectory/$name-ui.json"
 $status=Call 'session.status';$control=$status.world.runtimeControl
 Require ($control.paused -and $control.controlMode -eq 'paused' -and $control.pendingSteps -eq 0 -and !$control.stepInProgress) 'HUD comparison simulation is not paused'
 $script:receipts+=@(@{name=$name;upscaling=$u;taaActive=$raster.taaActive;simulationFrame=$world.applicationSimulationFrame;
   runtimeControl=$control;simulationTimeSeconds=$status.simulationTimeSeconds})
 Write-Host "$GraphicsApi HUD $name captured and checked"
}
try {
 & $session @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 2560x1440 -StartupScene $StartupScene *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause';$null=Call 'runtime.step' @{frames=30}
 Settings @{'Rendering.Upscaling.Mode'='Off';'Rendering.Resolution.RenderHeight'=0;'Rendering.AntiAliasing.Mode'='TAA';
  'Rendering.PostProcess.AutoExposure.Enabled'=$false;'Rendering.PostProcess.ExposureCompensationEV'=0}
 Capture '1440-native' $false
 Settings @{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Performance'}
 Capture '1440-fsr' $true
 $passes=@();$offset=0;$snapshot=$null
 do {
  $p=@{section='passes';offset=$offset;limit=100};if($snapshot){$p.snapshotId=$snapshot}
  $page=Call 'render.graph.snapshot' $p;$passes+=@($page.items);$offset=$page.nextOffset;$snapshot=$page.snapshotId
 }while($null -ne $offset)
 $passes|ConvertTo-Json -Depth 16|Set-Content "$OutputDirectory/passes.json"
 $null=Call 'window.resize' @{width=3840;height=2160};$expectedSize=@(3840,2160)
 Capture '2160-fsr' $true
 $before=Call 'render.settings.get'
 $native=Call 'render.settings.apply' @{baseRevision=$before.revision;baseHash=$before.hash;patch=@{'Rendering.Upscaling.Mode'='Off'}}
 $native|ConvertTo-Json -Depth 16|Set-Content "$OutputDirectory/native4k-apply.json"
 $native4KRejected=!$native.success
 if($native.success){Capture '2160-native' $false}
 else {
  Require $AllowNative4KRejection.IsPresent ($native|ConvertTo-Json -Depth 8 -Compress)
  $after=Call 'render.settings.get'
  @{before=$before;after=$after}|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/rejection-settings.json"
  Require ($after.revision -eq $before.revision -and $after.hash -eq $before.hash -and $after.effective.'Rendering.Upscaling.Mode' -eq 'FSR3.1') 'Rejected native 4K transaction changed live settings'
  # More than all three frame banks must remain usable after candidate rejection.
  $null=Call 'capture.frame' @{filename='rollback-bank-cycle.png';delayFrames=12}
  Capture '2160-after-rejection' $true
 }
 $finished=$true
}catch {$failure=$_.Exception.Message;throw}
finally {
 try {
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   Require (Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet) 'Game exit not verified'
   $shutdownVerified=$true
  }
  if($owner){
   Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log"
   Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"
   $log=Get-Content "$OutputDirectory/runtime.log" -Raw
   Require ($log -notmatch 'Validation Error|ScriptException') 'Runtime or shutdown validation error'
   Require ((Get-Item "$OutputDirectory/stderr.log").Length -eq 0) 'Runtime stderr is nonempty'
  }
  if($finished -and $shutdownVerified){$state='success'}
 }catch {$failure=$_.Exception.Message;throw}
 finally {
  try {
   $receipts|ConvertTo-Json -Depth 16|Set-Content "$OutputDirectory/receipts.json"
   @{status=$state;api=$GraphicsApi;native4KRejected=$native4KRejected;shutdownVerified=$shutdownVerified;
     failure=$failure;exeSha256=$ExistingBuildSha256;pid=if($owner){$owner.pid}else{0};
     runDirectory=if($owner){$owner.runDirectory}else{''}}|ConvertTo-Json -Depth 8|Set-Content "$OutputDirectory/result.json"
  }finally {Complete-HEStorageRun -Run $storage -State $state}
 }
}
