[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [Parameter(Mandatory=$true)][string]$BaselineRoot,
 [string]$FinalRoot='',
 [string]$IsolatedProject='',
 [Parameter(Mandatory=$true)][string]$BaselineBinary,
 [Parameter(Mandatory=$true)][string]$FinalBinary,
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$BaselineSha256,
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$FinalSha256,
 [Parameter(Mandatory=$true)][string]$BaselineBuildResult,
 [Parameter(Mandatory=$true)][string]$FinalBuildResult,
 [string]$Python='python'
)
$ErrorActionPreference='Stop'
if(!$FinalRoot){$FinalRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))}
$BaselineRoot=[IO.Path]::GetFullPath($BaselineRoot);$FinalRoot=[IO.Path]::GetFullPath($FinalRoot)
if(!$IsolatedProject){$IsolatedProject=Join-Path $BaselineRoot 'Project'}
$IsolatedProject=[IO.Path]::GetFullPath($IsolatedProject)
if($IsolatedProject -eq [IO.Path]::GetFullPath((Join-Path $FinalRoot 'Project'))){throw 'Refusing the user final Project; use the isolated baseline Project.'}
if(!(Test-Path (Join-Path $IsolatedProject 'Project.heproject'))){throw 'Isolated project descriptor missing.'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path $OutputDirectory){throw 'Use a new output directory.'}
New-Item -ItemType Directory $OutputDirectory | Out-Null
. (Join-Path $FinalRoot 'Tools/Agent/Storage.ps1')
$run=Start-HEStorageRun -Project $IsolatedProject -Workflow 'baseline-final-ab' -Paths @($OutputDirectory) -Outer
$records=[Collections.Generic.List[object]]::new();$state='failed'
$envKeys=@('HE_PROJECT_DIR','HE_RENDER_FULL_SURFACE_DATA','HE_RENDER_DISABLE_MATERIAL_PACK_CACHE','HE_RENDER_MERGE_GI_DEPTH_PYRAMID');$old=@{}
foreach($key in $envKeys){$old[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$settings=@{'Rendering.Path'='Forward';'Rendering.Shadows.Enabled'=$true;'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='Off';'Rendering.AO.Enabled'=$true;'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.GpuBatching.Enabled'=$true;'Rendering.GpuBatching.ShadowsEnabled'=$true;'Rendering.Memory.TransientTextureReuse'=$true;'Rendering.Presentation.VSync'=$false;'Rendering.Debug.RenderGraph.Validation'=$false}
$settingsPath=Join-Path $OutputDirectory 'requested-settings.json'
[IO.File]::WriteAllText($settingsPath,($settings|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$endpoints=@{baseline=@{root=$BaselineRoot;binary=$BaselineBinary;hash=$BaselineSha256};final=@{root=$FinalRoot;binary=$FinalBinary;hash=$FinalSha256}}
function Save-CaptureRuntimeLogs([string]$ControlLog, [string]$EndpointRoot, [string]$ArchiveDirectory) {
 # Called after bounded capture returns, before another run can reclaim old logs.
 New-Item -ItemType Directory -Path $ArchiveDirectory -Force | Out-Null
 $entries=[Collections.Generic.List[object]]::new()
 $matches=@()
 if(Test-Path -LiteralPath $ControlLog -PathType Leaf){
  $raw=Get-Content -LiteralPath $ControlLog -Raw
  $matches=@([regex]::Matches($raw,'(?m)^run HGame: STARTED pid=\d+; stdout: (?<stdout>[^\r\n]+?); stderr: (?<stderr>[^\r\n]+)'))
 }
 $jail=[IO.Path]::GetFullPath((Join-Path $EndpointRoot 'out/agent')).TrimEnd('\')+'\'
 foreach($kind in @('stdout','stderr')){
  $entry=[ordered]@{kind=$kind;status='unavailable';source=$null;copy=$null;sourceSha256=$null;copySha256=$null;reason=$null}
  if($matches.Count -ne 1){$entry.reason='Expected exactly one STARTED stdout/stderr record in control log.'}
  else {
   try {
    $source=[IO.Path]::GetFullPath($matches[0].Groups[$kind].Value.Trim())
    $entry.source=$source
    if(!$source.StartsWith($jail,[StringComparison]::OrdinalIgnoreCase)){$entry.reason='Source rejected: outside endpoint out/agent jail.'}
    elseif(!(Test-Path -LiteralPath $source -PathType Leaf)){$entry.reason='Source log missing, possibly reclaimed; no replacement evidence.'}
    else {
     $destination=Join-Path $ArchiveDirectory ('game.'+$kind+'.log')
     Copy-Item -LiteralPath $source -Destination $destination -Force
     $entry.copy=$destination
     $entry.sourceSha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
     $entry.copySha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
     if($entry.sourceSha256 -eq $entry.copySha256){$entry.status='archived'}
     else{$entry.status='hash_mismatch';$entry.reason='Source and archived bytes differ.'}
    }
   } catch {$entry.reason=$_.Exception.Message;$entry.status='copy_failed'}
  }
  $entries.Add($entry)
 }
 $manifest=[ordered]@{controlLog=$ControlLog;endpointRoot=$EndpointRoot;archivedUtc=[DateTime]::UtcNow.ToString('o');files=@($entries.ToArray());note='Copies of existing runtime logs, not additional GPU samples. Missing logs remain unavailable.'}
 [IO.File]::WriteAllText((Join-Path $ArchiveDirectory 'manifest.json'),($manifest|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
}
function Save-Hardware([string]$Path) {
 $info=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');gpuStatus='unavailable';powerPlanStatus='unavailable'}
 if(Get-Command nvidia-smi -ErrorAction SilentlyContinue){$info.gpu=@(& nvidia-smi --query-gpu=name,driver_version,temperature.gpu,power.draw,clocks.sm --format=csv 2>&1 | ForEach-Object{[string]$_});$info.gpuStatus=if($LASTEXITCODE -eq 0){'available'}else{'query_failed'}}
 if(Get-Command powercfg -ErrorAction SilentlyContinue){$info.powerPlan=@(& powercfg /getactivescheme 2>&1 | ForEach-Object{[string]$_});$info.powerPlanStatus=if($LASTEXITCODE -eq 0){'available'}else{'query_failed'}}
 [IO.File]::WriteAllText($Path,($info|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
$buildResults=@{baseline=$BaselineBuildResult;final=$FinalBuildResult}
$workloads=@{LocalLights='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he';Sponza='Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD/IntelSponzaDaylight.he';SurfaceCoverage='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he'}
try {
 foreach($key in $envKeys){[Environment]::SetEnvironmentVariable($key,$null,'Process')}
 # Both use their ordinary defaults. New-only benchmark overrides remain unset.
 & $Python (Join-Path $PSScriptRoot 'Analyze-PipelineVersions.py') manifest $IsolatedProject (Join-Path $OutputDirectory 'inputs-before.json')
 if($LASTEXITCODE){throw 'Input manifest failed.'}
 foreach($side in @('baseline','final')){
  $ep=$endpoints[$side]
  $receipt=Get-Content -LiteralPath $buildResults[$side] -Raw | ConvertFrom-Json
  if($receipt.target -ne 'HGame' -or $receipt.configuration -ne 'RelWithDebInfo' -or $receipt.state -ne 'success' -or
     [IO.Path]::GetFullPath($receipt.project) -ne $IsolatedProject -or
     [IO.Path]::GetFullPath($receipt.artifact) -ne [IO.Path]::GetFullPath($ep.binary)){throw "$side Build result does not prove this project/artifact."}
  Copy-Item -LiteralPath $buildResults[$side] -Destination (Join-Path $OutputDirectory "$side-build-result.json")
  $cache=Join-Path $receipt.buildRoot 'CMakeCache.txt'
  if(!(Test-Path $cache)){throw "$side CMakeCache missing."}
  Get-Content -LiteralPath $cache | Where-Object {$_ -match '^(CMAKE_(BUILD_TYPE|CXX_COMPILER|CXX_COMPILER_VERSION|GENERATOR|MAKE_PROGRAM|CXX_FLAGS|LINKER)|HE_|PACK_GAME|WITH_EDITOR|PROJECT_)'} | Set-Content (Join-Path $OutputDirectory "$side-cmake-summary.txt")
  Get-ChildItem (Join-Path $receipt.buildRoot 'CMakeFiles') -Filter CMakeCXXCompiler.cmake -Recurse | Select-Object -First 1 | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $OutputDirectory "$side-compiler.cmake")}
  & git -C $ep.root rev-parse HEAD | Set-Content (Join-Path $OutputDirectory "$side-head.txt")
  $deltaPath=Join-Path $OutputDirectory "$side-tracked.patch"
  & git -C $ep.root diff --binary HEAD "--output=$deltaPath"
  if($LASTEXITCODE){throw 'Could not preserve tracked source delta.'}
 }
 for($repeat=0;$repeat -lt 3;$repeat++){
  foreach($workload in @('LocalLights','Sponza','SurfaceCoverage')){
   $order=@('baseline','final');if($repeat % 2){[array]::Reverse($order)}
   foreach($side in $order){
    $ep=$endpoints[$side];$name="$workload-$side-$repeat";$prefix=Join-Path $OutputDirectory $name
    $before=(Get-FileHash -LiteralPath $ep.binary -Algorithm SHA256).Hash
    if($before -ne $ep.hash){throw "$side binary differs from pinned hash."}
    $args=@('game','capture','-Configuration','RelWithDebInfo','-Project',$IsolatedProject,'-GraphicsApi','DX12','-StrictGraphicsApi','-Resolution','1920x1080','-FixedDeltaSeconds','0.0166667','-StartupScene',$workloads[$workload],'-RenderSettingsPath',$settingsPath,'-PerformanceWarmupFrames','600','-Frames','2400','-RenderStats','-RenderStatsPath',($prefix+'.csv'),'-CapturePath',($prefix+'.png'),'-TimeoutSeconds','600')
    # Bounded capture owns/waits/reaps. Separate repository cwd prevents fallback asset roots.
    Save-Hardware ($prefix+'-hardware-before.json')
    $savedStorageParent=[Environment]::GetEnvironmentVariable('HE_STORAGE_PARENT','Process')
    $crossStore=[IO.Path]::GetFullPath($ep.root).TrimEnd('\') -ine [IO.Path]::GetFullPath($FinalRoot).TrimEnd('\')
    Push-Location $ep.root
    try {
     if($crossStore){[Environment]::SetEnvironmentVariable('HE_STORAGE_PARENT',$null,'Process')}
     & (Join-Path $ep.root 'hectl.bat') @args *> ($prefix+'.log');$code=$LASTEXITCODE
    } finally { [Environment]::SetEnvironmentVariable('HE_STORAGE_PARENT',$savedStorageParent,'Process');Pop-Location }
    Save-CaptureRuntimeLogs ($prefix+'.log') $ep.root ($prefix+'-runtime-logs')
    Save-Hardware ($prefix+'-hardware-after.json')
    $artifactLines=@(Get-Content ($prefix+'.log') | Where-Object {$_ -match '^artifact:\s*(.+)$'} | ForEach-Object {($_ -replace '^artifact:\s*','').Trim()})
    if($artifactLines.Count -ne 1 -or [IO.Path]::GetFullPath($artifactLines[0]) -ne [IO.Path]::GetFullPath($ep.binary)){throw 'Actual Build artifact log does not match the pinned artifact.'}
    $after=(Get-FileHash -LiteralPath $ep.binary -Algorithm SHA256).Hash
    $record=@{name=$name;side=$side;workload=$workload;repeat=$repeat;repository=$ep.root;project=$IsolatedProject;scene=$workloads[$workload];binaryPath=$ep.binary;binaryBeforeSha256=$before;binarySha256=$after;exitCode=$code;arguments=$args;warmupFrames=600;measureFrames=1800;sceneSha256=(Get-FileHash (Join-Path $IsolatedProject $workloads[$workload]) -Algorithm SHA256).Hash}
    $records.Add($record)
    if($code -ne 0 -or $after -ne $ep.hash){throw "Capture $name failed or changed pinned binary."}
    $observed=Get-Content ($prefix+'.csv.settings.json') -Raw | ConvertFrom-Json
    $mismatches=@()
    foreach($key in $settings.Keys){
     $section=if($key.StartsWith('Rendering.Debug.')){'debug'}else{'effective'}
     $property=$observed.$section.PSObject.Properties[$key]
     if(!$property -or $null -eq $property.Value -or $property.Value -ne $settings[$key]){$mismatches+=@{field=$key;expected=$settings[$key];actual=$property.Value}}
    }
    $record.settingsMismatches=$mismatches
    if($mismatches.Count){throw ($mismatches|ConvertTo-Json -Compress)}
    if(!(Test-Path ($prefix+'.png'))){throw 'Image missing.'}
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
   }
  }
 }
 & $Python (Join-Path $PSScriptRoot 'Analyze-PipelineVersions.py') manifest $IsolatedProject (Join-Path $OutputDirectory 'inputs-after.json')
 if($LASTEXITCODE){throw 'Final input manifest failed.'}
 if((Get-FileHash (Join-Path $OutputDirectory 'inputs-before.json')).Hash -ne (Get-FileHash (Join-Path $OutputDirectory 'inputs-after.json')).Hash){throw 'Isolated source inputs changed; invalidate comparisons.'}
 & $Python (Join-Path $PSScriptRoot 'Analyze-PipelineVersions.py') analyze $OutputDirectory
 if($LASTEXITCODE){throw 'Analysis failed.'}
 $state='success'
}finally{
 foreach($key in $envKeys){[Environment]::SetEnvironmentVariable($key,$old[$key],'Process')}
 [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
 Complete-HEStorageRun -Run $run -State $state
}
