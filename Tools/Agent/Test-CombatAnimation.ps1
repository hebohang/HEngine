[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
 [string]$StartupScene='Assets/Scenes/SickaDynasty.he'
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed'}
$context=($contextJson -join "`n")|ConvertFrom-Json
$session=Join-Path $PSScriptRoot 'Session.ps1'
$exe=Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe')
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;ArtifactPath=$exe;
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=240;ExistingBuildSha256=$ExistingBuildSha256}
$scene=Get-Content (Join-Path $context.project.Root $StartupScene) -Raw|ConvertFrom-Json
$ids=@{};foreach($entry in $scene.PSObject.Properties){if($entry.Value.TagComponent){$ids[$entry.Value.TagComponent.Tag]=$entry.Name}}
foreach($tag in @('Player','MechanicGirlVisual','Camera')){if(!$ids[$tag]){throw "Scene has no $tag"}}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'combat-animation' -Paths @($OutputDirectory) -Outer
$started=$false;$owner=$null;$state='failed';$finished=$false;$samples=@();$failure=$null
function Call([string]$method,[hashtable]$p=@{}) {
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){throw ($r|ConvertTo-Json -Depth 12 -Compress)}
 return $r.result
}
function Step([int]$frames){$null=Call 'runtime.step' @{frames=$frames}}
function Held([string[]]$keys){$null=Call 'input.set' @{held=@($keys);onStep=$true}}
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Sample([string]$name){
 $capture=Call 'capture.frame' @{filename="$name.png";delayFrames=2}
 Copy-Item $capture.path "$OutputDirectory/$name.png"
 Copy-Item ($capture.path+'.world.json') "$OutputDirectory/$name.world.json"
 $world=Get-Content ($capture.path+'.world.json') -Raw|ConvertFrom-Json
 Require ($world.recordedRaster.upscaling.active -and !$world.recordedRaster.upscaling.failure -and !$world.recordedRaster.taaActive) "$name FSR/TAA state invalid"
 $actor=(Call 'render.animation.status').objects|Where-Object {$_.uuid -eq $ids.MechanicGirlVisual}|Select-Object -First 1
 Require ($actor -and $actor.controller.loaded -and $actor.currentPaletteCount -gt 0 -and $actor.nonFiniteComparisons -eq 0 -and $actor.controller.nonFiniteParameters -eq 0) "$name actor diagnostics invalid"
 $camera=Call 'world.get' @{uuid=$ids.Camera;components=@('CameraComponent')}
 $player=Call 'world.get' @{uuid=$ids.Player;components=@('TransformComponent')}
 $sample=@{name=$name;frame=$world.applicationSimulationFrame;actor=$actor;
   cameraFov=$camera.components.CameraComponent.Camera.mPerspectiveFOV;playerWorld=$player.worldMatrix}
 $script:samples+=@($sample)
 $script:samples|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/samples.json"
 Write-Host "$GraphicsApi $name state=$($actor.controller.stateName) transition=$($actor.controller.inTransition)"
 return $sample
}
function Phase([string]$name,[string[]]$keys,[int]$frames){Held $keys;Step $frames;return Sample $name}
try {
 & $session @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -StartupScene $StartupScene *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause';$settings=Call 'render.settings.get'
 $apply=Call 'render.settings.apply' @{baseRevision=$settings.revision;baseHash=$settings.hash;patch=@{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Quality';'Rendering.GI.Technique'='Off'}}
 Require $apply.success 'FSR settings failed'
 $idle=Phase '01-idle' @() 20
 $null=Phase '02-aim-start' @('q') 1
 $aim=Phase '03-aim-settled' @('q') 20
 Require ($aim.cameraFov -gt 0 -and $aim.cameraFov -lt $idle.cameraFov) 'Aim did not narrow camera FOV'
 $right=Phase '04-aim-strafe' @('q','d') 20
 Require ($right.actor.controller.parameters.MoveX -gt .2) 'Aimed strafe did not produce lateral motion'
 $forward=Phase '05-aim-forward' @('q','w') 20
 Require ($forward.actor.controller.parameters.MoveY -gt .2) 'Aimed forward motion missing'
 $null=Phase '06-sprint-start' @('w','left_shift') 1
 $null=Phase '07-sprint-transition' @('w','left_shift') 6
 $sprint=Phase '08-sprint-settled' @('w','left_shift') 24
 Require $sprint.actor.controller.parameters.Sprint 'Sprint animation parameter never became active'
 $null=Phase '09-walk-transition' @('w') 4
 $walk=Phase '10-walk-settled' @('w') 16
 Require (!$walk.actor.controller.parameters.Sprint) 'Sprint animation persisted after release'
 $null=Phase '11-stop-transition' @() 4
 $stopped=Phase '12-stop-settled' @() 20
 Require ([Math]::Abs($stopped.actor.controller.parameters.MoveX) -lt .05 -and [Math]::Abs($stopped.actor.controller.parameters.MoveY) -lt .05) 'Locomotion did not settle'
 # Restart through the actual game input, keeping both action sequences short
 # enough to observe animation before combat can terminate the session.
 Held @('f5');Step 1;Held @();Step 30
 $fire=Phase '13-fire-onset' @('f') 1
 Require ($fire.actor.controller.parameters.FireWeight -gt 0) 'Fire layer absent'
 $null=Phase '14-fire-moving' @('f','d') 5
 $reload=Phase '15-reload-start' @('r','d') 1
 Require ($reload.actor.controller.parameters.ReloadWeight -gt 0) 'Reload layer absent'
 $null=Phase '16-reload-moving-transition' @('d') 6
 $mid=Phase '17-reload-moving' @('d') 30
 Require ($mid.actor.controller.parameters.ReloadWeight -gt .9 -and $mid.actor.controller.parameters.ReloadProgress -gt .1 -and $mid.actor.controller.parameters.ReloadProgress -lt .6) 'Reload clock/weight did not advance'
 $null=Phase '18-switch-cancel-start' @('2') 1
 $null=Phase '19-switch-cancel-transition' @() 6
 $switched=Phase '20-switch-settled' @() 14
 Require ($switched.actor.controller.parameters.ReloadWeight -lt .05) 'Cancelled reload layer did not fade'
 $null=Phase '21-repeater-fire' @('f') 1
 $null=Phase '22-repeater-reload' @('r') 1
 $null=Phase '23-reload-middle' @() 78
 $completed=Phase '24-reload-completed' @() 96
 Require ($completed.actor.controller.parameters.ReloadWeight -lt .05 -and $completed.actor.controller.parameters.ReloadProgress -ge .999) 'Reload failed to complete and fade'
 $null=Phase '25-holster-start' @('h') 1
 $holstered=Phase '26-holster-settled' @() 24
 Require (!$holstered.actor.controller.parameters.Aim) 'Holster left armed pose selected'
 $null=Phase '27-draw-start' @('1') 1
 $drawn=Phase '28-draw-settled' @() 24
 Require $drawn.actor.controller.parameters.Aim 'Draw did not restore armed pose'
 $log=Get-Content "$($owner.runDirectory)/game.stdout.log" -Raw
 foreach($required in @('Shooter shot:','Shooter reload cancelled;','Shooter reload completed;','Shooter weapon:')){Require $log.Contains($required) "Missing behavior: $required"}
 Require ($log -notmatch 'Shooter finish:|ScriptException|Validation Error') 'Unexpected combat completion or runtime error'
 $finished=$true
}catch {$failure=$_.Exception.Message;throw}
finally {
 try {
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   Require (Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet) 'Game exit not verified'
  }
  if($finished -and $started){$state='success'}
 }catch {$state='failed';$failure=$_.Exception.Message;throw}
 finally {
  try {
   @{status=$state;api=$GraphicsApi;pid=if($owner){$owner.pid}else{0};samples=$samples.Count;failure=$failure;
     exeSha256=$ExistingBuildSha256;scene=$StartupScene;scope='Actual Game input/controller/pose observations; paused captures are not continuous-motion image quality proof'}|ConvertTo-Json -Depth 8|Set-Content "$OutputDirectory/result.json"
   if($owner){Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log";Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"}
  }finally {Complete-HEStorageRun -Run $storage -State $state}
 }
}
