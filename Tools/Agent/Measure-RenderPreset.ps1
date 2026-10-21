[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][string]$CasesPath,
 [string]$StartupScene='Assets/Scenes/SickaDynasty.he',
 [ValidateRange(5,180)][int]$MeasureSeconds=12,
 [switch]$UseProjectDisplaySettings,
 [switch]$Gameplay,
 [switch]$AimCombat,
 [switch]$ExploreAfterCombat,
 [switch]$RespectPresentationSettings
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$cases=Get-Content -LiteralPath $CasesPath -Raw|ConvertFrom-Json
Copy-Item -LiteralPath $CasesPath -Destination "$OutputDirectory/cases.json"
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed'}
$context=($contextJson -join "`n")|ConvertFrom-Json
if($AimCombat -and !$Gameplay){throw 'AimCombat requires Gameplay'}
if($ExploreAfterCombat -and !$AimCombat){throw 'ExploreAfterCombat requires AimCombat'}
if($AimCombat){
 $scene=Get-Content (Join-Path $context.project.Root $StartupScene) -Raw|ConvertFrom-Json
 $ids=@{}
 foreach($p in $scene.PSObject.Properties){$ids[$p.Value.TagComponent.Tag]=$p.Name}
 $sensitivity=($scene.PSObject.Properties.Value|Where-Object {$_.TagComponent.Tag -eq 'Camera'}).FollowCameraComponent.MouseSensitivity
 if(!$sensitivity -or !$ids.Camera -or !$ids.ShooterSentry0){throw 'AimCombat requires the shooter scene'}
}
$session=Join-Path $PSScriptRoot 'Session.ps1'
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;
 ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'));
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=300;ExistingBuildSha256=$ExistingBuildSha256}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'render-preset-measure' -Paths @($OutputDirectory) -Outer
$started=$false;$owner=$null;$state='failed';$phases=@();$result=@{status='FAILED';exeSha256=$ExistingBuildSha256;gameplay=[bool]$Gameplay}
function Call([string]$method,[hashtable]$p=@{}){
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){throw ($r|ConvertTo-Json -Depth 8 -Compress)}
 return $r.result
}
function Aim-CombatTarget {
 $target=$null
 foreach($i in 0..2){
  $candidate=Call 'world.get' @{uuid=$ids["ShooterSentry$i"];components=@('TransformComponent')}
  if($candidate.components.TransformComponent.Scale[0] -ne 0){$target=$candidate;break}
 }
 if(!$target){return $false}
 $null=Call 'input.pointer' @{x=1920;y=1080;rightHeld=$false}
 $camera=Call 'world.get' @{uuid=$ids.Camera;components=@('TransformComponent')}
 $m=$camera.worldMatrix;$t=$target.worldMatrix
 $dx=$t[12]-$m[12];$dy=$t[13]-$m[13];$dz=$t[14]-$m[14]
 $length=[Math]::Sqrt($dx*$dx+$dy*$dy+$dz*$dz)
 $yaw=[Math]::Atan2(-$m[8],-$m[10]);$pitch=[Math]::Asin([Math]::Max(-1.0,[Math]::Min(1.0,[double](-$m[9]))))
 $yawDelta=[Math]::Atan2([Math]::Sin([Math]::Atan2($dx,$dz)-$yaw),[Math]::Cos([Math]::Atan2($dx,$dz)-$yaw))
 $pitchDelta=[Math]::Asin($dy/$length)-$pitch
 $null=Call 'input.pointer' @{x=[Math]::Max(1,[Math]::Min(3839,1920-$yawDelta/$sensitivity));y=[Math]::Max(1,[Math]::Min(2159,1080-$pitchDelta/$sensitivity));rightHeld=$true}
 return $true
}
try {
 $display=if($UseProjectDisplaySettings){@{UseProjectDisplaySettings=$true}}else{@{Resolution='3840x2160'}}
 $saved=Join-Path $context.project.Root 'Saved/Config'
 if(Test-Path -LiteralPath $saved){$display.SavedConfigSource=$saved}
 & $session @common @display -Action Start -Realtime:$Gameplay -RespectPresentationSettings:$RespectPresentationSettings -GraphicsApi DX12 -StrictGraphicsApi -StartupScene $StartupScene -RenderStats -RenderStatsPath "$OutputDirectory/frames.csv" -PerformanceWarmupFrames 30 *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause';$null=Call 'runtime.step' @{frames=30}
 (Call 'render.settings.get')|ConvertTo-Json -Depth 32|Set-Content "$OutputDirectory/startup-settings.json"
 foreach($case in $cases){
  if($case.name -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid case name'}
  $settings=Call 'render.settings.get'
  $r=Call 'render.settings.apply' @{baseRevision=$settings.revision;baseHash=$settings.hash;patch=$case.patch}
  if(!$r.success){throw ($r|ConvertTo-Json -Depth 8 -Compress)}
  if($Gameplay){
   $null=Call 'input.set' @{held=@('f5');onStep=$true};$null=Call 'runtime.step' @{frames=1}
   $null=Call 'input.clear';$null=Call 'runtime.step' @{frames=30}
  }
  Start-Sleep -Seconds 8
  $before=Call 'session.status';$start=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $actions=@()
  if($Gameplay){
   $fullHealth=Call 'ui.element.rect' @{id='healthbar'}
   $null=Call 'runtime.resume'
   $timer=[Diagnostics.Stopwatch]::StartNew();$index=0;$emptyTargets=0;$exploring=$false
   $inputs=@(@('q','d','f'),@('q','a','f'),@('r','d'),@('w','left_shift'),@('s','a'),@('s','d','f'),@('2'),@('r','a'))
   while($timer.Elapsed.TotalSeconds -lt $MeasureSeconds){
    $keys=$inputs[$index % $inputs.Count]
    if($AimCombat -and !$exploring){
     $hasTarget=Aim-CombatTarget
     $emptyTargets=if($hasTarget){0}else{$emptyTargets+1}
     $keys=if($index % 5 -eq 4 -or !$hasTarget){@('r')}else{@('f')}
     if($ExploreAfterCombat -and $emptyTargets -ge 6){$exploring=$true}
    }
    if($exploring){
     $keys=@(@('w'),@('w'),@('d'),@('s'),@('s'),@('a'),@('a'),@('d'))[$index % 8]
     $null=Call 'input.pointer' @{x=1920;y=1080;rightHeld=$false}
     $null=Call 'input.pointer' @{x=1921;y=1080;rightHeld=$true;steps=45;stepX=1;stepY=0}
    }
    $null=Call 'input.set' @{held=@($keys)}
    if(!$AimCombat -and $index % 4 -eq 0){$null=Call 'input.pointer' @{x=1800;y=1000;rightHeld=$true;steps=120;stepX=2;stepY=0}}
    Start-Sleep -Milliseconds $(if($AimCombat){650}else{2000})
    $health=Call 'ui.element.rect' @{id='healthbar'}
    $reset=$health.rect[2] -lt $fullHealth.rect[2]*.35
    $actions+=@{keys=@($keys);utcMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();exploring=$exploring;healthbar=$health;resetAfterSample=$reset;runtime=(Call 'session.status')}
    if($reset -and !$AimCombat){
     # Keep measuring active combat rather than an inexpensive defeated scene.
     # Include the normal F5 retry cost; record every reset in the receipt.
     $null=Call 'input.set' @{held=@('f5')};Start-Sleep -Milliseconds 150
     $null=Call 'input.clear'
    }
    if($reset -and $AimCombat){throw 'Combat route failed to keep the player alive; do not treat it as acceptance'}
    $index++
   }
  }else{Start-Sleep -Seconds $MeasureSeconds}
  $end=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();$after=Call 'session.status'
  if($Gameplay){$null=Call 'input.clear';$null=Call 'runtime.pause'}
  $capture=Call 'capture.frame' @{filename=($case.name+'.png');delayFrames=4}
  Copy-Item -LiteralPath $capture.path -Destination "$OutputDirectory/$($case.name).png"
  Copy-Item -LiteralPath ($capture.path+'.world.json') -Destination "$OutputDirectory/$($case.name).world.json"
  $world=Get-Content ($capture.path+'.world.json') -Raw|ConvertFrom-Json
  if($world.recordedRaster.upscaling.failure){throw 'FSR failure'}
  $effective=Call 'render.settings.get'
  $effective|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/$($case.name)-settings.json"
  $phases+=@{name=$case.name;startUtcMs=$start;endUtcMs=$end;before=$before;after=$after;actions=$actions;recordedRaster=$world.recordedRaster}
  $phases|ConvertTo-Json -Depth 32|Set-Content "$OutputDirectory/phases.json"
  Write-Host "$($case.name): measured $MeasureSeconds seconds"
 }
 $result.status='CAPTURED'
}finally {
 try {
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   if(!(Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet)){throw 'Shutdown not verified'}
   $result.pid=$owner.pid;$result.shutdownVerified=$true
   Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log"
   Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"
   if((Get-Item "$OutputDirectory/stderr.log").Length -ne 0){throw 'Nonempty runtime stderr'}
   if(Select-String -LiteralPath "$OutputDirectory/runtime.log" -Pattern 'Validation Error|ScriptException' -Quiet){throw 'Runtime error'}
   if($result.status -eq 'CAPTURED'){$result.status='CAPTURED_AND_STOPPED';$state='success'}
  }
 }finally {
  $result|ConvertTo-Json -Depth 8|Set-Content "$OutputDirectory/result.json"
  Complete-HEStorageRun -Run $storage -State $state
 }
}
if($state -ne 'success'){throw 'Measurement failed'}
