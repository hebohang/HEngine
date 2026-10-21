[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12'
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$json=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed'}
$context=($json -join "`n")|ConvertFrom-Json
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;
 ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'));
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=240;ExistingBuildSha256=$ExistingBuildSha256}
$session=Join-Path $PSScriptRoot 'Session.ps1'
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'fsr-portability' -Paths @($OutputDirectory) -Outer
$started=$false;$finished=$false;$owner=$null;$state='failed';$failure=$null;$samples=@()
$expectedSize=@(1280,720)
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Call([string]$method,[hashtable]$p=@{}) {
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 Require $r.ok ($r|ConvertTo-Json -Depth 8 -Compress)
 return $r.result
}
function Settings([hashtable]$patch){
 $before=Call 'render.settings.get'
 $result=Call 'render.settings.apply' @{baseRevision=$before.revision;baseHash=$before.hash;patch=$patch}
 Require $result.success 'Settings apply failed'
 $effective=(Call 'render.settings.get').effective
 foreach($key in $patch.Keys){
  $matches=if($patch[$key] -is [double] -or $patch[$key] -is [single]){
   [Math]::Abs([double]$effective.$key-[double]$patch[$key]) -lt .00001
  }else {$effective.$key -eq $patch[$key]}
  Require $matches "Setting override: $key"
 }
}
function Step([int]$frames){
 while($frames -gt 0){$count=[Math]::Min(120,$frames);$null=Call 'runtime.step' @{frames=$count};$frames-=$count}
}
function Sample([string]$name,[string]$quality='Quality',[bool]$active=$true){
 $capture=Call 'capture.frame' @{filename="$name.png";delayFrames=8}
 $world=Get-Content ($capture.path+'.world.json') -Raw|ConvertFrom-Json
 $r=$world.recordedRaster;$u=$r.upscaling
 Require ($r.valid -and $u.active -eq $active -and $r.taaActive -ne $active) "$name FSR/TAA selection invalid"
 if($active){
  $ratio=@{NativeAA=1.0;Quality=1.5;Balanced=1.7;Performance=2.0;UltraPerformance=3.0}[$quality]
  Require (!$u.failure -and $u.quality -eq $quality -and $u.preExposure -eq 1) "$name upscaler state invalid"
  for($i=0;$i -lt 2;++$i){Require ($u.outputSize[$i] -eq $expectedSize[$i] -and $u.renderSize[$i] -eq [Math]::Floor([single]$expectedSize[$i]/[single]$ratio)) "$name extent invalid"}
 }
 Require ($r.textureStreaming.failedUploads -eq 0) "$name upload failed"
 Copy-Item -LiteralPath $capture.path -Destination "$OutputDirectory/$name.png"
 Copy-Item -LiteralPath ($capture.path+'.world.json') -Destination "$OutputDirectory/$name.world.json"
 $script:samples+=@(@{name=$name;upscaling=$u;streaming=$r.textureStreaming;simulationFrame=$world.applicationSimulationFrame})
 Write-Host "$GraphicsApi $name PASS residency=$($r.textureStreaming.residentPendingRetiringBytes)"
 return $r.textureStreaming
}
try {
 & $session @common -Action Start -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -StartupScene Assets/Scenes/Main.he *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause'
 Settings @{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Quality';'Rendering.Upscaling.Sharpness'=0.2;
   'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='Off';'Rendering.TextureStreaming.BudgetMiB'=512}
 Step 420
 $baseline=Sample '01-baseline'
 Require ($baseline.textures -gt 0 -and $baseline.residentPendingRetiringBytes -gt 1048576) 'Fixture did not create meaningful texture demand'
 $log=Get-Content "$($owner.runDirectory)/game.stdout.log" -Raw
 foreach($marker in @('PORTABILITY navigation ready','PORTABILITY state Acquire','PORTABILITY state Pursue','PORTABILITY state Windup','PORTABILITY state Recover','PORTABILITY PASS attacks=2 commits=2 completed=2')){
  Require $log.Contains($marker) "Missing independent-project behavior: $marker"
 }
 Settings @{'Rendering.TextureStreaming.BudgetMiB'=1}
 Step 240
 $pressure=Sample '02-pressure'
 Require ($pressure.budgetBytes -eq 1048576 -and $pressure.residentPendingRetiringBytes -lt $baseline.residentPendingRetiringBytes -and $pressure.transitions -gt $baseline.transitions) 'Texture budget pressure did not cause mip residency change'
 Settings @{'Rendering.TextureStreaming.BudgetMiB'=512}
 Step 240
 $recovery=Sample '03-recovery'
 Require ($recovery.residentPendingRetiringBytes -gt $pressure.residentPendingRetiringBytes) 'Texture residency did not recover'
 foreach($quality in @('NativeAA','Balanced','Performance','UltraPerformance')){
  Settings @{'Rendering.Upscaling.Quality'=$quality}
  $null=Sample $quality $quality
 }
 Settings @{'Rendering.Upscaling.Mode'='Off'}
 $null=Sample 'Off' 'Quality' $false
 Settings @{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Performance';'Rendering.Upscaling.Sharpness'=1.0}
 foreach($size in @(@(2560,1440),@(3840,2160),@(1280,720))){
  $null=Call 'window.resize' @{width=$size[0];height=$size[1]};$expectedSize=$size
  $null=Sample ('resize-'+($size -join 'x')) 'Performance'
 }
 $finished=$true
}catch {$failure=$_.Exception.Message;throw}
finally {
 try {
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   Require (Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet) 'Game exit not verified'
  }
  if($owner){
   Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log"
   Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"
   $log=Get-Content "$OutputDirectory/runtime.log" -Raw
   Require ($log -notmatch 'ScriptException|Validation Error|PORTABILITY.*Exception') 'Runtime error logged'
   Require ((Get-Item "$OutputDirectory/stderr.log").Length -eq 0) 'Runtime stderr is nonempty'
  }
  if($finished -and $started){$state='success'}
 }catch {$failure=$_.Exception.Message;throw}
 finally {
  try {
   @{status=$state;failure=$failure;api=$GraphicsApi;project=$context.project.Root;projectId=$context.project.ProjectId;
     exeSha256=$ExistingBuildSha256;pid=if($owner){$owner.pid}else{0};samples=$samples}|
     ConvertTo-Json -Depth 20|Set-Content "$OutputDirectory/result.json"
  }finally {Complete-HEStorageRun -Run $storage -State $state}
 }
}
