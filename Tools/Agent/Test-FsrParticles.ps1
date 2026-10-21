[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][string]$ExistingBuildSha256,
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
 [switch]$GpuProbe
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$sessionTool=Join-Path $PSScriptRoot 'Session.ps1'
$contextJson=& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') project current -Project $Project -Json
if($LASTEXITCODE -ne 0){throw 'Project resolution failed.'}
$context=($contextJson -join "`n")|ConvertFrom-Json
$common=@{Configuration='RelWithDebInfo';ProjectRoot=$context.project.Root;SessionRoot=$context.agentSessions;
 TimeoutSeconds=300;OwnerTag=[guid]::NewGuid().ToString('N');
 ArtifactPath=(Join-Path $context.gameBuild ('bin/'+$context.project.ExecutableName+'.exe'))}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun=Start-HEStorageRun -Project $context.project.Root -Workflow 'fsr-particles' -Paths @($OutputDirectory) -Outer
$state='failed';$started=$false;$owner=$null;$receipts=[Collections.Generic.List[object]]::new()
$previousProbe=$env:HE_VFX_GPU_PARTICLES
$cases=@('blend','add','subtract','multiply','distortion')
if($GpuProbe){$env:HE_VFX_GPU_PARTICLES='1';$cases+= 'gpu-add'}
function Call([string]$method,[hashtable]$p=@{}) {
 $lines=@(& $sessionTool @common -Action Call -Method $method -ParamsJson ($p|ConvertTo-Json -Depth 16 -Compress))
 $r=($lines|Select-Object -Last 1)|ConvertFrom-Json
 if(!$r.ok){throw ($r|ConvertTo-Json -Depth 12 -Compress)}
 return $r.result
}
function Step([int]$frames){$null=Call 'runtime.step' @{frames=$frames}}
function Control([long]$handle,[string]$action,[hashtable]$extra=@{}) {
 $p=@{handle=$handle;action=$action};foreach($key in $extra.Keys){$p[$key]=$extra[$key]}
 $r=Call 'vfx.control' $p
 if(!$r.accepted){throw "VFX $action rejected"}
 return $r
}
function Capture([string]$phase) {
 foreach($source in @('upscale-color','upscale-opaque','reactive','composition')) {
  $name="$phase-$source.raw";$r=Call 'capture.frame' @{filename=$name;source=$source;delayFrames=2}
  foreach($suffix in @('','.json','.world.json')){Copy-Item ($r.path+$suffix) "$OutputDirectory/$name$suffix"}
  $w=Get-Content ($r.path+'.world.json') -Raw|ConvertFrom-Json
  if(!$w.recordedRaster.upscaling.active -or $w.recordedRaster.upscaling.failure -or $w.recordedRaster.taaActive){throw "Invalid FSR/TAA state at $phase"}
  $receipts.Add(@{phase=$phase;source=$source;receipt=$r})
 }
 $r=Call 'capture.frame' @{filename="$phase.png";delayFrames=1};Copy-Item $r.path "$OutputDirectory/$phase.png"
}
try {
 & $sessionTool @common -Action Start -ExistingBuildSha256 $ExistingBuildSha256 -GraphicsApi $GraphicsApi -StrictGraphicsApi -RenderValidation -Resolution 960x540 -StartupScene 'Assets/Diagnostics/FsrParticles/Particles.he' *> "$OutputDirectory/start.log"
 $started=$true;$owner=Get-Content "$($common.SessionRoot)/active.json" -Raw|ConvertFrom-Json
 $null=Call 'runtime.pause';$s=Call 'render.settings.get'
 $r=Call 'render.settings.apply' @{baseRevision=$s.revision;baseHash=$s.hash;patch=@{'Rendering.Upscaling.Mode'='FSR3.1';'Rendering.Upscaling.Quality'='Quality';'Rendering.GI.Technique'='Off'}}
 if(!$r.success){throw 'FSR settings rejected'}
 foreach($method in @('vfx.describe','vfx.stats','vfx.quality')){(Call $method)|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/$method.json"}
 $hashes=@{executable=$ExistingBuildSha256;api=$GraphicsApi;pid=$owner.pid;gpuProbe=[bool]$GpuProbe;cases=$cases;scene=(Get-FileHash "$Project/Assets/Diagnostics/FsrParticles/Particles.he").Hash}
 Capture 'empty'
 foreach($name in $cases) {
  $path="Assets/Diagnostics/FsrParticles/$name.efkpkg"
  $hashes[$name]=(Get-FileHash "$Project/$path").Hash
  $play=Call 'vfx.play' @{path=$path;position=@(-.65,0,2);seed=1806;scale=1}
  if(!$play.playing){throw "Could not play $name"}
  Step 6;$null=Control $play.handle 'pause';Capture "$name-peak"
  $stats=Call 'vfx.stats';$stats|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/$name-stats.json"
  if($name -eq 'gpu-add') {
   if(!$stats.gpuEnabled -or $stats.lastGpuSimulationSubmission -le 0){throw 'GPU probe did not submit simulation'}
   $null=Control $play.handle 'color' @{value=@(0,0,0,1)}
  } else {$null=Control $play.handle 'transform' @{position=@(.65,0,2);forward=@(0,0,1)}}
  # Effekseer applies root transforms during simulation, not paused rendering.
  $null=Control $play.handle 'resume';Step 1;$null=Control $play.handle 'pause'
  if($name -eq 'gpu-add'){Capture "$name-zero"}else{Capture "$name-moved"}
  $null=Control $play.handle 'resume';Step 120;Step 8
  $ended=Call 'vfx.control' @{handle=$play.handle;action='status'}
  if($ended.playing){throw "$name did not end naturally"}
  Capture "$name-ended"
  $play=Call 'vfx.play' @{path=$path;position=@(0,0,2);seed=1806;scale=1};Step 6
  $null=Control $play.handle 'stop';Step 3
  if((Call 'vfx.stats').activeEffects.current -ne 0){throw "$name retained an active effect after stop"}
  Write-Host "$GraphicsApi $name lifecycle PASS"
 }
 Capture 'stopped'
 $hashes|ConvertTo-Json|Set-Content "$OutputDirectory/inputs.json"
 $state='success'
} finally {
 try {
  $receipts|ConvertTo-Json -Depth 24|Set-Content "$OutputDirectory/receipts.json"
  if($started){& $sessionTool @common -Action Stop *> "$OutputDirectory/stop.log"}
  if($owner){Copy-Item "$($owner.runDirectory)/game.stdout.log" "$OutputDirectory/runtime.log";Copy-Item "$($owner.runDirectory)/game.stderr.log" "$OutputDirectory/stderr.log"}
 } finally {$env:HE_VFX_GPU_PARTICLES=$previousProbe;Complete-HEStorageRun -Run $storageRun -State $state}
}
Write-Host "runtime-manual PASS: $OutputDirectory (pixel analysis/visual review separate)"
