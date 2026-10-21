[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [ValidateSet('Balanced','Performance')][string]$DefaultQuality='Balanced'
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
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=300;ExistingBuildSha256=$ExistingBuildSha256}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow '4k-preset-ui' -Paths @($OutputDirectory) -Outer
$started=$false;$state='failed';$owner=$null;$stage='initial';$saved=''
function Call([string]$method,[hashtable]$p=@{}){
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){throw "$method failed: $($r.error.message)"}
 return $r.result
}
function Click([string]$id){
 $e=Call 'ui.element.rect' @{id=$id}
 if(!$e.visible){throw "Missing UI $id"}
 $r=$e.rect
 $null=Call 'input.click' @{x=$r[0]+$r[2]*.5;y=$r[1]+$r[3]*.5;button='left'}
 $null=Call 'runtime.step' @{frames=24}
}
function Check([string]$name,[string]$quality){
 $settings=Call 'render.settings.get'
 $settings|ConvertTo-Json -Depth 32|Set-Content "$OutputDirectory/$name-settings.json"
 $e=$settings.effective
 if($e.'Rendering.Upscaling.Mode' -ne 'FSR3.1' -or $e.'Rendering.Upscaling.Quality' -ne $quality -or $e.'Rendering.TextureStreaming.BudgetMiB' -ne 1536){throw "$name effective preset differs"}
 if(!$e.'Rendering.Presentation.VSync'){throw 'Normal startup must retain project VSync'}
 $capture=Call 'capture.frame' @{filename="$name.png";delayFrames=8}
 $world=Get-Content ($capture.path+'.world.json') -Raw|ConvertFrom-Json
 $u=$world.recordedRaster.upscaling
 $height=if($quality -eq 'Balanced'){1270}else{1080}
 if(!$u.active -or $u.failure -or $u.outputSize[0] -ne 3840 -or $u.outputSize[1] -ne 2160 -or $u.renderSize[1] -ne $height){throw "$name actual upscaling differs"}
 Copy-Item $capture.path "$OutputDirectory/$name.png"
 Copy-Item ($capture.path+'.world.json') "$OutputDirectory/$name.world.json"
}
function Stop-Owned{
 & $session @common -Action Stop *> "$OutputDirectory/$stage-stop.log"
 if(!(Select-String -LiteralPath "$OutputDirectory/$stage-stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet)){throw 'Shutdown not verified'}
 $script:started=$false
 Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/$stage-runtime.log"
 Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/$stage-stderr.log"
 if((Get-Item "$OutputDirectory/$stage-stderr.log").Length -ne 0 -or (Select-String -LiteralPath "$OutputDirectory/$stage-runtime.log" -Pattern 'Validation Error|ScriptException' -Quiet)){throw 'Runtime error'}
}
try{
 & $session @common -Action Start -UseProjectDisplaySettings -RespectPresentationSettings -SavedConfigSource (Join-Path $context.project.Root 'Saved/Config') -GraphicsApi DX12 -StrictGraphicsApi *> "$OutputDirectory/initial-start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.step' @{frames=60}
 Click 'settings';Check 'default' $DefaultQuality
 Click 'quality';Click 'choice-0';Check 'menu-performance' 'Performance'
 Click 'quality';Click 'choice-1';Check 'menu-balanced' 'Balanced'
 Click 'texturebudget';Click 'choice-4';Check 'texture-1536' 'Balanced'
 if($DefaultQuality -eq 'Performance'){
  # The texture dropdown scrolls the settings list down. Return through the
  # menu to bring the quality row back into the clipped scroll viewport.
  Click 'back';Click 'settings';Click 'quality';Click 'choice-0';Check 'final-default' $DefaultQuality
 }
 $saved=Join-Path $owner.runDirectory 'Saved/Config'
 Copy-Item (Join-Path $saved 'UserEngine.json') "$OutputDirectory/persisted-UserEngine.json"
 Stop-Owned
 $stage='restart';$common.OwnerTag=[guid]::NewGuid().ToString('N')
 & $session @common -Action Start -UseProjectDisplaySettings -RespectPresentationSettings -SavedConfigSource $saved -GraphicsApi DX12 -StrictGraphicsApi *> "$OutputDirectory/restart-start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.step' @{frames=60}
 Click 'settings';Check 'restart' $DefaultQuality
 Stop-Owned
 $state='success'
}finally{
 try{
  if($started){Stop-Owned}
  @{state=$state;exeSha256=$ExistingBuildSha256}|ConvertTo-Json|Set-Content "$OutputDirectory/result.json"
 }finally{Complete-HEStorageRun -Run $storage -State $state}
}
