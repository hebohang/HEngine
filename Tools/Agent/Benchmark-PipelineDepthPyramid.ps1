[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExistingBuildSha256,
 [string]$RepositoryRoot='',
 [ValidateSet('Correctness','Performance')][string]$Mode='Performance',
 [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
 [string]$Python='python'
)
$ErrorActionPreference='Stop'
if(!$RepositoryRoot){$RepositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))}
$RepositoryRoot=[IO.Path]::GetFullPath($RepositoryRoot)
$warmup=if($Mode -eq 'Performance'){600}else{0}
$measure=if($Mode -eq 'Performance'){1800}else{64}
$repeats=if($Mode -eq 'Performance'){3}else{1}
$apis=if($Mode -eq 'Correctness'){@('DX12','Vulkan')}else{@($GraphicsApi)}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $RepositoryRoot 'Tools/Agent/Storage.ps1')
# Extract only the checked settings comparator; never execute the capture script.
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $RepositoryRoot 'Tools/Agent/Test-PipelineSsgiConvergence.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Settings comparator source did not parse.'}
$helper=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-SsgiSettingsMismatches'},$true)
if(!$helper){throw 'Settings comparator not found.'}
. ([scriptblock]::Create($helper.Extent.Text))
$run=Start-HEStorageRun -Project (Join-Path $RepositoryRoot 'Project') -Workflow 'depth-pass-merge-ab' -Paths @($OutputDirectory) -Outer
$state='failed';$records=[Collections.Generic.List[object]]::new()
$envNames=@('HE_RENDER_MERGE_GI_DEPTH_PYRAMID','HE_RENDER_FULL_SURFACE_DATA','HE_RENDER_DISABLE_MATERIAL_PACK_CACHE')
$previous=@{};foreach($key in $envNames){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$scene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he'
$binary=Join-Path $RepositoryRoot 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
$sceneHash=(Get-FileHash (Join-Path $RepositoryRoot ('Project/'+$scene)) -Algorithm SHA256).Hash
$settings=@{'Rendering.Path'='Forward';'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='ScreenSpaceGI';'Rendering.GI.Denoiser'='SVGF';'Rendering.GI.RenderScale'=0.5;'Rendering.GI.AtrousIterations'=2;'Rendering.GI.BilateralUpsample'=$true;'Rendering.GI.SSGI.RayCount'=1;'Rendering.GI.SSGI.TemporalWeight'=0.97;'Rendering.GI.SSGI.StepCount'=64;'Rendering.GI.SSGI.MaximumRayDistance'=10.0;'Rendering.GI.SSGI.Thickness'=0.1;'Rendering.GI.SSGI.BinaryRefinementSteps'=3;'Rendering.GI.SSGI.HiZStartMip'=2;'Rendering.GI.SVGF.OptimizedKernels'=$true;'Rendering.GI.MaxBounces'=1;'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.Presentation.VSync'=$false;'Rendering.Debug.GI.View'=0;'Rendering.Debug.GI.DirectOnly'=$false;'Rendering.Debug.RenderGraph.Validation'=($Mode -eq 'Correctness')}
$settingsPath=Join-Path $OutputDirectory 'requested-settings.json'
[IO.File]::WriteAllText($settingsPath,($settings|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
try {
 [Environment]::SetEnvironmentVariable('HE_RENDER_FULL_SURFACE_DATA','0','Process')
 [Environment]::SetEnvironmentVariable('HE_RENDER_DISABLE_MATERIAL_PACK_CACHE','0','Process')
 foreach($api in $apis){
 for($repeat=0;$repeat -lt $repeats;$repeat++){
  $modes=@(0,1);if($repeat % 2){[array]::Reverse($modes)}
  foreach($mergeValue in $modes){
   $name="merge-$mergeValue-$repeat";if($Mode -eq 'Correctness'){$name="$api-$name"};$prefix=Join-Path $OutputDirectory $name
   $before=(Get-FileHash $binary -Algorithm SHA256).Hash
   if($before -ne $ExistingBuildSha256){throw 'Pinned binary changed before capture.'}
   [Environment]::SetEnvironmentVariable('HE_RENDER_MERGE_GI_DEPTH_PYRAMID',"$mergeValue",'Process')
   $arguments=@('game','capture','-Configuration','RelWithDebInfo','-GraphicsApi',$api,'-StrictGraphicsApi','-FixedDeltaSeconds','0.0166667','-Resolution','1920x1080','-StartupScene',$scene,'-RenderSettingsPath',$settingsPath,'-RenderStats','-RenderStatsPath',($prefix+'.csv'),'-PerformanceWarmupFrames',"$warmup",'-Frames',"$($warmup+$measure)",'-CapturePath',($prefix+'.png'),'-TimeoutSeconds','360')
   if($Mode -eq 'Correctness'){$arguments += '-RenderValidation'}
   & (Join-Path $RepositoryRoot 'hectl.bat') @arguments *> ($prefix+'.log')
   $code=$LASTEXITCODE
   $after=(Get-FileHash $binary -Algorithm SHA256).Hash
   $record=@{name=$name;repeat=$repeat;merge=$mergeValue;exitCode=$code;binaryBeforeSha256=$before;binarySha256=$after;sceneSha256=$sceneHash;graphicsApi=$api;arguments=$arguments;warmupFrames=$warmup;measureFrames=$measure;mode=$Mode;imageStatus='NOT_REVIEWED'}
   $records.Add($record)
   if($code -ne 0){throw "Capture $name failed."}
   if($after -ne $ExistingBuildSha256){throw 'Capture build changed pinned binary.'}
   if((Get-FileHash (Join-Path $RepositoryRoot ('Project/'+$scene)) -Algorithm SHA256).Hash -ne $sceneHash){throw 'Source scene changed.'}
   $effective=Get-Content ($prefix+'.csv.settings.json') -Raw | ConvertFrom-Json
   $mismatches=@(Get-SsgiSettingsMismatches $settings $effective)
   $record.settingsMismatches=$mismatches
   if($mismatches.Count){throw ($mismatches|ConvertTo-Json -Depth 8 -Compress)}
   $rows=@(Import-Csv ($prefix+'.csv')|Where-Object {[long]$_.frame -gt $warmup})
   if($rows.Count -ne $measure -or !$rows[0].PSObject.Properties['cpuGraphRecordMs']){throw 'CPU sample count or Graph.Execute metric missing.'}
   $passNames=@(Import-Csv ($prefix+'.csv.passes.csv')|Select-Object -ExpandProperty passName -Unique)
   $pyramid=@($passNames|Where-Object {$_ -match '^DepthPyramid(Mip0|ReduceMip[0-9]+|Merged)$'})
   $record.observedPyramidPassNames=$pyramid
   $pyramid | ConvertTo-Json | Set-Content ($prefix+'-observed-graph-pass-names.json') -Encoding utf8
   if($mergeValue -eq 1){if($pyramid.Count -ne 1 -or $pyramid[0] -ne 'DepthPyramidMerged'){throw 'Merged graph path not observed.'}}
   else {if($pyramid.Count -ne 11 -or $pyramid -contains 'DepthPyramidMerged' -or $pyramid -notcontains 'DepthPyramidMip0'){throw 'Expected 11 legacy mip passes not observed.'}}
   if(!(Test-Path ($prefix+'.png'))){throw 'Capture image missing.'}
   [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
  }
 }
 }
 if($Mode -eq 'Performance'){
 & $Python (Join-Path $PSScriptRoot 'Analyze-DepthPassMerge.py') $OutputDirectory
 if($LASTEXITCODE -ne 0){throw 'Offline analysis failed.'}
 for($repeat=0;$repeat -lt 3;$repeat++){
  & $Python (Join-Path $RepositoryRoot 'Tools/Agent/Compare-RenderingImages.py') (Join-Path $OutputDirectory "merge-0-$repeat.png") (Join-Path $OutputDirectory "merge-1-$repeat.png") --output (Join-Path $OutputDirectory "image-difference-$repeat.json")
  if($LASTEXITCODE -ne 0){throw 'Image comparison unavailable/failed; inspect evidence.'}
 }
 } else {
 foreach($api in $apis){
 & $Python (Join-Path $RepositoryRoot 'Tools/Agent/Compare-RenderingImages.py') (Join-Path $OutputDirectory "$api-merge-0-0.png") (Join-Path $OutputDirectory "$api-merge-1-0.png") --output (Join-Path $OutputDirectory "$api-image-difference.json")
 if($LASTEXITCODE -ne 0){throw 'Correctness image comparison failed.'}
 }
 }
 $state='success'
}finally{
 foreach($key in $envNames){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}
 [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
 Complete-HEStorageRun -Run $run -State $state
}
Write-Host 'Sampling complete. Image metrics are not a quality PASS; review captures and paired timings.'
