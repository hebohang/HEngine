[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [ValidateSet('Balanced','Performance')][string]$Quality='Balanced'
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed'}
$context=($contextJson -join "`n")|ConvertFrom-Json
$session=Join-Path $PSScriptRoot 'Session.ps1'
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;
 ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'));
 SessionRoot=$context.agentSessions;OwnerTag=[guid]::NewGuid().ToString('N');TimeoutSeconds=300;ExistingBuildSha256=$ExistingBuildSha256}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storage=Start-HEStorageRun -Project $context.project.Root -Workflow 'fsr-4k-quality' -Paths @($OutputDirectory) -Outer
$started=$false;$owner=$null;$state='failed';$receipts=@()
function Call([string]$method,[hashtable]$p=@{}){
 $lines=@(& $session @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){$r|ConvertTo-Json -Depth 16|Set-Content "$OutputDirectory/failure.json";throw "$method failed; see failure.json"}
 return $r.result
}
try {
 & $session @common -Action Start -UseProjectDisplaySettings -SavedConfigSource (Join-Path $context.project.Root 'Saved/Config') -GraphicsApi DX12 -StrictGraphicsApi -StartupScene 'Assets/Scenes/SickaDynasty.he' *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause'
 # Compression preserves original capture bytes and paths without duplicating 4K frames.
 $captures=Join-Path $owner.sessionDirectory 'captures'
 New-Item -ItemType Directory -Path $captures -Force|Out-Null
 & compact.exe /C /Q $captures *> "$OutputDirectory/compression.log"
 if($LASTEXITCODE -ne 0){throw 'Capture compression setup failed'}
 foreach($quality in @($Quality)){
  $settings=Call 'render.settings.get'
  $profiles=Get-Content (Join-Path $context.project.Root 'Config/RenderQuality.json') -Raw|ConvertFrom-Json
  $profile=$profiles.Profiles|Where-Object {$_.Id -eq $quality.ToLowerInvariant()}|Select-Object -First 1
  if(!$profile){throw "Missing project quality profile: $quality"}
  $applied=Call 'render.settings.apply' @{baseRevision=$settings.revision;baseHash=$settings.hash;patch=$profile.Patch}
  if(!$applied.success){throw 'Settings rejected'}
  $load=Call 'runtime.scene.load' @{path='Assets/Scenes/SickaDynasty.he';forceReload=$true}
  $ready=$false
  for($attempt=0;$attempt -lt 60;$attempt++){
   $summary=Call 'world.summary'
   if($summary.sceneGeneration -gt $load.previousSceneGeneration){$ready=$true;break}
   Start-Sleep -Milliseconds 100
  }
  if(!$ready){throw 'Scene reload not completed'}
  $null=Call 'runtime.pause';$null=Call 'runtime.step' @{frames=30}
  Start-Sleep -Seconds 8
  foreach($phase in @('static','move','turn')){
   if($phase -eq 'move'){$null=Call 'input.set' @{held=@('d');onStep=$true}}
   if($phase -eq 'turn'){
    $null=Call 'input.clear'
    $null=Call 'input.pointer' @{x=1920;y=1080;rightHeld=$false}
    $null=Call 'runtime.step' @{frames=1}
    $null=Call 'input.pointer' @{x=1922;y=1080;rightHeld=$true;steps=16;stepX=2;stepY=0}
   }
   $args=@{prefix="$quality-$phase";frames=16}
   if($phase -eq 'static'){$args.cameraCut=$true;$args.settleFrames=64}
   $sequence=Call 'capture.sequence' $args
   foreach($frame in $sequence.frames){
    $world=Get-Content ($frame.path+'.world.json') -Raw|ConvertFrom-Json
    if($world.recordedRaster.upscaling.failure){throw 'FSR failure'}
   }
   $receipts+=@{quality=$quality;phase=$phase;sequence=$sequence}
   $receipts|ConvertTo-Json -Depth 32|Set-Content "$OutputDirectory/sequences.json"
   $newFiles=@($sequence.frames|ForEach-Object {$_.path;($_.path+'.world.json')})
   & compact.exe /C /F /EXE:LZX /Q @newFiles *> "$OutputDirectory/compression-$phase.log"
   if($LASTEXITCODE -ne 0){throw 'Capture compression failed'}
   Write-Host "$quality $phase captured"
  }
  $null=Call 'input.clear'
 }
 $state='success'
}finally{
 try{
  if($started){
   & $session @common -Action Stop *> "$OutputDirectory/stop.log"
   if(!(Select-String -LiteralPath "$OutputDirectory/stop.log" -SimpleMatch "agent session: STOPPED pid=$($owner.pid)" -Quiet)){throw 'Shutdown not verified'}
   Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log"
   Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"
  }
  @{state=$state;pid=$owner.pid;exeSha256=$ExistingBuildSha256}|ConvertTo-Json|Set-Content "$OutputDirectory/result.json"
 }finally{Complete-HEStorageRun -Run $storage -State $state}
}
