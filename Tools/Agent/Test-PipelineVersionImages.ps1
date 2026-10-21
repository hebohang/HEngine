[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [string]$BaselineRoot='',[string]$FinalRoot='',
 [Parameter(Mandatory=$true)][string]$ProjectRoot,
 [ValidateSet('baseline','final','both')][string]$Side='both',
 [string]$BaselineArtifact='', [string]$FinalArtifact='',
 [string]$BaselineSha256='', [string]$FinalSha256=''
)
$ErrorActionPreference='Stop'
if(!$FinalRoot){$FinalRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))}
if($Side -ne 'final' -and !$BaselineRoot){throw 'BaselineRoot is required when selecting the baseline.'}
$selectedSides=if($Side -eq 'both'){@('baseline','final')}else{@($Side)}
foreach($selectedSide in $selectedSides){
 $selectedArtifact=if($selectedSide -eq 'baseline'){$BaselineArtifact}else{$FinalArtifact}
 $selectedHash=if($selectedSide -eq 'baseline'){$BaselineSha256}else{$FinalSha256}
 if([string]::IsNullOrWhiteSpace($selectedArtifact) -or $selectedHash -notmatch '^[a-fA-F0-9]{64}$'){throw "$selectedSide requires an explicit Artifact and exactly 64 hexadecimal SHA256 characters."}
 if(!(Test-Path -LiteralPath $selectedArtifact -PathType Leaf)){throw "$selectedSide artifact is missing."}
}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot)
if(Test-Path $OutputDirectory){throw 'Use new output.'};New-Item -ItemType Directory $OutputDirectory | Out-Null
. (Join-Path $FinalRoot 'Tools/Agent/Storage.ps1')
$run=Start-HEStorageRun -Project $ProjectRoot -Workflow 'baseline-final-motion-ssgi' -Paths @($OutputDirectory) -Outer
$records=[Collections.Generic.List[object]]::new();$state='failed';$sequence=0
$envNames=@('HE_RENDER_FULL_SURFACE_DATA','HE_RENDER_DISABLE_MATERIAL_PACK_CACHE','HE_RENDER_MERGE_GI_DEPTH_PYRAMID');$savedEnv=@{};foreach($k in $envNames){$savedEnv[$k]=[Environment]::GetEnvironmentVariable($k,'Process');[Environment]::SetEnvironmentVariable($k,$null,'Process')}
function Write-Json([string]$path,$value){[IO.File]::WriteAllText($path,($value|ConvertTo-Json -Depth 32),[Text.UTF8Encoding]::new($false))}
function Invoke-Session([string]$action,[string]$method='session.status',[hashtable]$parameters=@{},[string[]]$extra=@()){
 $script:sequence++;$prefix=Join-Path $script:caseDir ('call-{0:d4}' -f $script:sequence)
 $paramPath=$prefix+'-params.json';Write-Json $paramPath $parameters
 # A wrapper file avoids PowerShell 5 native command-line JSON escaping entirely.
 $wrapper=$prefix+'.ps1'
 $invoke=@{Action=$action;ProjectRoot=$ProjectRoot;ArtifactPath=$script:ep.artifact;SessionRoot=$script:sessionRoot;OwnerTag=$script:owner;Configuration='RelWithDebInfo';ExistingBuildSha256=$script:ep.hash;TimeoutSeconds=180}
 if($action -eq 'Call'){$invoke.Method=$method;$invoke.ParamsJson=($parameters|ConvertTo-Json -Depth 20 -Compress)}
 if($action -eq 'Start'){$invoke.StartupScene=$script:scene;$invoke.GraphicsApi='DX12';$invoke.StrictGraphicsApi=$true;$invoke.RenderValidation=$true;$invoke.Resolution='1280x720';$invoke.FixedDeltaSeconds=0.0166667;$invoke.RenderStats=$true;$invoke.RenderStatsPath=(Join-Path $script:caseDir 'stats.csv');$invoke.RenderSettingsPath=$script:settingsPath}
 Write-Json ($prefix+'-invoke.json') $invoke
 $escapedSession=(Join-Path $script:ep.root 'Tools/Agent/Session.ps1').Replace("'","''");$escapedInvoke=($prefix+'-invoke.json').Replace("'","''")
 $clearCrossStore=if([IO.Path]::GetFullPath($script:ep.root).TrimEnd('\') -ine [IO.Path]::GetFullPath($FinalRoot).TrimEnd('\')){"[Environment]::SetEnvironmentVariable('HE_STORAGE_PARENT',`$null,'Process')"}else{''}
 @"
`$ErrorActionPreference='Stop'
$clearCrossStore
`$obj=Get-Content -LiteralPath '$escapedInvoke' -Raw|ConvertFrom-Json
`$argsTable=@{};foreach(`$p in `$obj.PSObject.Properties){`$argsTable[`$p.Name]=`$p.Value}
& '$escapedSession' @argsTable
"@ | Set-Content -LiteralPath $wrapper -Encoding utf8
 $started=[DateTime]::UtcNow.ToString('o');$process=Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$wrapper+'"')) -RedirectStandardOutput ($prefix+'.stdout.log') -RedirectStandardError ($prefix+'.stderr.log') -WindowStyle Hidden -PassThru
 try{$null=$process.Handle;$process.WaitForExit();$code=$process.ExitCode}finally{$process.Dispose()}
 $raw=Get-Content ($prefix+'.stdout.log') -Raw
 Write-Json ($prefix+'-receipt.json') @{action=$action;method=$method;startedUtc=$started;finishedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$code;invocation=$invoke}
 if($code -ne 0){throw "Session $action/$method failed; $prefix"}
 if($action -ne 'Call'){return}
 $index=$raw.IndexOf('{');if($index -lt 0){throw 'No JSON response.'}
 $response=$raw.Substring($index)|ConvertFrom-Json
 if(!$response.ok){throw 'Agent rejected request.'};return $response.result
}
function Archive-OwnedSession([string]$phase){
 $activePath=Join-Path $script:sessionRoot 'active.json'
 if(!(Test-Path -LiteralPath $activePath)){return}
 $active=Get-Content -LiteralPath $activePath -Raw|ConvertFrom-Json
 if($active.ownerTag -ne $script:owner){throw 'Refusing another owner session archive.'}
 $runPath=[IO.Path]::GetFullPath([string]$active.runDirectory)
 $jail=[IO.Path]::GetFullPath($script:sessionRoot).TrimEnd('\')+'\'
 if(!$runPath.StartsWith($jail,[StringComparison]::OrdinalIgnoreCase)){throw 'Session run directory escaped owned root.'}
 $archivePath=Join-Path $script:caseDir ('session-archive-'+$phase)
 New-Item -ItemType Directory -Path $archivePath -Force|Out-Null
 $items=[Collections.Generic.List[object]]::new()
 $sources=@((Join-Path $runPath 'session.json'),(Join-Path $runPath 'mailbox/session.json'))
 # The live game holds log handles: archive logs only after owned Stop returns.
 if($phase -eq 'after-stop'){$sources+=@((Join-Path $runPath 'game.stdout.log'),(Join-Path $runPath 'game.stderr.log'))}
 foreach($source in $sources){
  if(!(Test-Path -LiteralPath $source -PathType Leaf)){continue}
  $relative=$source.Substring($runPath.Length).TrimStart('\').Replace('\','-')
  $destination=Join-Path $archivePath $relative
  $input=[IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  try{$output=[IO.File]::Create($destination);try{$input.CopyTo($output)}finally{$output.Dispose()}}finally{$input.Dispose()}
  $copyHash=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
  $sourceHash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
  $items.Add(@{source=$source;copy=$destination;sourceSha256=$sourceHash;copySha256=$copyHash;exactAtHashCheck=($sourceHash -eq $copyHash)})
 }
 Write-Json (Join-Path $archivePath 'manifest.json') @{phase=$phase;runDirectory=$runPath;sessionDirectory=$active.sessionDirectory;ownerTag=$active.ownerTag;state=$active.state;files=@($items.ToArray());note='Before-stop archives metadata only; logs are copied and hashed after owned Stop. No source files removed.'}
}
function Step-Idle([int]$frames){
 $step=Invoke-Session 'Call' 'runtime.step' @{frames=$frames}
 $status=Invoke-Session 'Call' 'session.status'
 if([long]$status.world.runtimeControl.completedSteps -lt [long]$step.targetCompletedSteps -or [long]$status.world.runtimeControl.pendingSteps -ne 0 -or [bool]$status.world.runtimeControl.stepInProgress){throw 'Step completion/idle barrier not reached.'}
 return $status
}
function Save-Capture([string]$name,[int]$delay){
 $before=Invoke-Session 'Call' 'session.status'
 $capture=Invoke-Session 'Call' 'capture.frame' @{filename=($name+'.png');delayFrames=$delay}
 $source=[IO.Path]::GetFullPath([string]$capture.path);$jail=[IO.Path]::GetFullPath($script:sessionRoot).TrimEnd('\')+'\'
 if(!$source.StartsWith($jail,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetExtension($source) -ne '.png' -or !(Test-Path -LiteralPath $source -PathType Leaf)){throw 'Capture outside owned session jail or missing.'}
 Copy-Item -LiteralPath $source -Destination (Join-Path $script:caseDir ($name+'.png'))
 $after=Invoke-Session 'Call' 'session.status'
 Write-Json (Join-Path $script:caseDir ($name+'-observation.json')) @{before=$before;capture=$capture;after=$after;requestedRenderDelay=$delay;note='Delayed rendered-frame endpoint, not atomic consecutive capture; shell gaps and startup history differ.'}
 $passes=@();$offset=0;$snapshot=$null
 do{$request=@{section='passes';offset=$offset;limit=100};if($snapshot){$request.snapshotId=$snapshot};$page=Invoke-Session 'Call' 'render.graph.snapshot' $request;$passes+=@($page.items);$offset=$page.nextOffset;$snapshot=$page.snapshotId}while($null -ne $offset)
 Write-Json (Join-Path $script:caseDir ($name+'-passes.json')) @($passes)
}
try{
 foreach($selectedSide in $selectedSides){
  $sideLabel=$selectedSide
  $ep=if($sideLabel -eq 'baseline'){@{root=$BaselineRoot;artifact=$BaselineArtifact;hash=$BaselineSha256}}else{@{root=$FinalRoot;artifact=$FinalArtifact;hash=$FinalSha256}}
  if((Get-FileHash $ep.artifact -Algorithm SHA256).Hash -ne $ep.hash){throw 'Artifact pin mismatch.'}
  $cases=@(@{name='surface-motion';scene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he';rays=0;delay=16})
  foreach($rays in @(1,8)){foreach($delay in @(128,512)){$cases+=@{name="ssgi-$rays-$delay";scene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he';rays=$rays;delay=$delay}}}
  foreach($case in $cases){
   $caseDir=Join-Path $OutputDirectory ($sideLabel+'-'+$case.name);New-Item -ItemType Directory $caseDir|Out-Null
   $owner=[guid]::NewGuid().ToString('N');$sessionRoot=Join-Path $ep.root ('out/agent/sessions/bf-'+$owner.Substring(0,8));$scene=$case.scene
   $settings=@{'Rendering.Path'='Forward';'Rendering.Shadows.Enabled'=$true;'Rendering.AO.Enabled'=$true;'Rendering.GpuBatching.Enabled'=$true;'Rendering.GpuBatching.ShadowsEnabled'=$true;'Rendering.Memory.TransientTextureReuse'=$true;'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'=$(if($case.rays){'ScreenSpaceGI'}else{'Off'});'Rendering.GI.Denoiser'='SVGF';'Rendering.GI.RenderScale'=0.5;'Rendering.GI.AtrousIterations'=2;'Rendering.GI.SSGI.RayCount'=[Math]::Max(1,$case.rays);'Rendering.GI.SSGI.TemporalWeight'=0.97;'Rendering.GI.SSGI.StepCount'=64;'Rendering.GI.SSGI.MaximumRayDistance'=10.0;'Rendering.GI.SSGI.Thickness'=0.1;'Rendering.GI.SSGI.BinaryRefinementSteps'=3;'Rendering.GI.SSGI.HiZStartMip'=2;'Rendering.GI.BilateralUpsample'=$true;'Rendering.GI.MaxBounces'=1;'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.Presentation.VSync'=$false;'Rendering.Debug.GI.View'=0;'Rendering.Debug.GI.DirectOnly'=$false;'Rendering.Debug.RenderGraph.Validation'=$true}
   $settingsPath=Join-Path $caseDir 'settings.json';Write-Json $settingsPath $settings
   $sceneJson=Get-Content (Join-Path $ProjectRoot $scene) -Raw|ConvertFrom-Json
   $camera=@($sceneJson.PSObject.Properties|Where-Object{$null -ne $_.Value.CameraComponent});if($camera.Count -ne 1){throw 'Need exactly one existing camera.'}
   $cameraId=$camera[0].Name;$base=@($camera[0].Value.TransformComponent.Translation)
   try{
    Invoke-Session 'Start'
    $null=Step-Idle 1
    $observed=Invoke-Session 'Call' 'render.settings.get';Write-Json (Join-Path $caseDir 'settings-observed.json') $observed
    foreach($key in $settings.Keys){
     $section=if($key.StartsWith('Rendering.Debug.')){'debug'}else{'effective'};$property=$observed.$section.PSObject.Properties[$key]
     if(!$property -or $null -eq $property.Value){throw "Missing setting $section/$key"}
     $matches=if($settings[$key] -is [double]){[Math]::Abs([double]$property.Value-[double]$settings[$key]) -le 0.000001}else{$property.Value -eq $settings[$key]}
     if(!$matches){throw "Settings mismatch $section/$key"}
    }
    if($case.rays){$null=Invoke-Session 'Call' 'camera.cut';Save-Capture 'endpoint' $case.delay}
    else {
     for($i=0;$i -lt 8;$i++){
      $position=@(([double]$base[0]+0.05*$i),([double]$base[1]),([double]$base[2]))
      $null=Invoke-Session 'Call' 'render.validation.transport' @{uuid=$cameraId;position=$position}
      $null=Step-Idle 1
      Save-Capture ('pose-'+$i) 16
     }
    }
    $records.Add(@{side=$sideLabel;case=$case.name;artifact=$ep.artifact;hash=$ep.hash;project=$ProjectRoot;scene=$scene;status='CAPTURED_NOT_REVIEWED'})
   }finally{try{Archive-OwnedSession 'before-stop'}finally{try{Invoke-Session 'Stop'}finally{Archive-OwnedSession 'after-stop'}}}
  }
 }
 $state='success'
}finally{foreach($k in $envNames){[Environment]::SetEnvironmentVariable($k,$savedEnv[$k],'Process')};Write-Json (Join-Path $OutputDirectory 'runs.json') @($records.ToArray());Complete-HEStorageRun -Run $run -State $state}
