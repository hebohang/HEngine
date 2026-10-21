[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidateRange(8,16)][int]$Frames=16,
    [ValidateRange(1,120)][int]$WarmupFrames=30,
    [hashtable]$CaseWarmupFrames=@{},
    [string]$DistortionEffectPath='',
    [string]$ParticleEffectPath='',
    [ValidateSet('gpu20k','cpu20k-proxy')][string]$VfxScenario='gpu20k',
    [string]$ExistingBuildSha256='',
    [string]$PythonExecutable='python',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$null=Get-Command $PythonExecutable -ErrorAction Stop
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
if($DistortionEffectPath -and !(Test-Path -LiteralPath (Join-Path $repositoryRoot ('Project/'+$DistortionEffectPath)) -PathType Leaf)){throw 'Distortion effect asset is absent.'}
if($ParticleEffectPath -and !(Test-Path -LiteralPath (Join-Path $repositoryRoot ('Project/'+$ParticleEffectPath)) -PathType Leaf)){throw 'Particle effect asset is absent.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N'); $sequence=0
$hectl=Join-Path $PSScriptRoot 'hectl.ps1'; $SampleMemory=$false; $expectedSize=@(1280,720)
$commands=[Collections.Generic.List[object]]::new(); $results=[Collections.Generic.List[object]]::new()
# Reuse only the checked transport/capture helpers, never execute the test driver.
$parseErrors=$null; $tokens=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-RenderingPaths.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Rendering helper source failed parsing.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Save-Capture')){
    $definition=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true) | Select-Object -First 1
    if(!$definition){throw "Missing helper: $name"}
    . ([scriptblock]::Create($definition.Extent.Text))
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$fixtureRelative='Assets/RenderingBenchmarks/TemporalValidation/'+$ownerTag
$fixtureRoot=Join-Path $repositoryRoot ('Project/'+$fixtureRelative)
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-temporal' -Paths @($OutputDirectory,$fixtureRoot) -Outer
$state='failed'; $savedEnvironment=@{}
$environment=@{HE_VFX_STRESS_MODE='1';HE_VFX_STRESS_SCENARIO=$VfxScenario;HE_VFX_STRESS_SEED='1806';HE_VFX_STRESS_CPU_MAX_HANDLES='8';HE_VFX_STRESS_CPU_EFFECT=''}
foreach($name in $environment.Keys){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$cases=@(@{name='vfx-baseline';source='Assets/Scenes/VFXStressTest.he';vfx=$false},
    @{name='surface';source='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he';vfx=$false},
    @{name='particles';source='Assets/Scenes/VFXStressTest.he';vfx=$true})
if($DistortionEffectPath){
    $cases+=@{name='distortion';source='Assets/Scenes/VFXStressTest.he';vfx=$true}
}
try {
    foreach($case in $cases){
        $sceneDirectory=Join-Path $OutputDirectory $case.name
        New-Item -ItemType Directory -Path $sceneDirectory | Out-Null
        $source=Join-Path $repositoryRoot ('Project/'+$case.source)
        $scene=Get-Content -LiteralPath $source -Raw | ConvertFrom-Json
        $cameraCount=0
        foreach($entity in $scene.PSObject.Properties){
            if($case.name -eq 'vfx-baseline'){$entity.Value.PSObject.Properties.Remove('PythonScriptComponent')}
            if($null -ne $entity.Value.CameraComponent){
                $entity.Value | Add-Member -NotePropertyName PythonScriptComponent -NotePropertyValue @{Path='Scripts.PBRShadowCaptureCamera.py'} -Force
                $cameraCount++
            }
        }
        if($cameraCount -ne 1){throw 'Temporal fixture requires exactly one camera.'}
        $currentScene=$fixtureRelative+'/'+$case.name+'.he'
        Write-Json (Join-Path $fixtureRoot ($case.name+'.he')) $scene
        foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name],'Process')}
        if($case.name -eq 'particles' -and $ParticleEffectPath){
            [Environment]::SetEnvironmentVariable('HE_VFX_STRESS_SCENARIO','cpu20k-proxy','Process')
            [Environment]::SetEnvironmentVariable('HE_VFX_STRESS_CPU_EFFECT',$ParticleEffectPath,'Process')
        }
        if($case.name -eq 'distortion'){
            [Environment]::SetEnvironmentVariable('HE_VFX_STRESS_SCENARIO','cpu20k-proxy','Process')
            [Environment]::SetEnvironmentVariable('HE_VFX_STRESS_CPU_EFFECT',$DistortionEffectPath,'Process')
        }
        # Restart each path so camera pose, effect age and random seed match.
        foreach($path in @('Forward','Deferred')){
          try {
            $arguments=@('agent','start','-StartupScene',$currentScene,'-Resolution','1280x720','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667')
            if($ExistingBuildSha256){$arguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
            $null=Invoke-HeCtl $arguments 'start' $false
            $null=Call-Agent 'render.view.create' @{width=1280;height=720}
                $settings=Call-Agent 'render.settings.get'
                $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$settings.revision;baseHash=$settings.hash;persist=$false;patch=@{
                    'Rendering.Path'=$path;'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='Off';'Rendering.GI.Denoiser'='None';
                    'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.GpuBatching.Enabled'=$true}}
                if(!$receipt.success){throw 'Temporal settings transaction failed.'}
                $caseWarmup=$(if($CaseWarmupFrames.ContainsKey($case.name)){[int]$CaseWarmupFrames[$case.name]}else{$WarmupFrames})
                if($caseWarmup -lt 0 -or $caseWarmup -gt 600){throw 'Case warmup must be 0..600 simulation steps.'}
                for($remainingWarmup=$caseWarmup;$remainingWarmup -gt 0;){
                    $warmupChunk=[Math]::Min(120,$remainingWarmup)
                    $null=Call-Agent 'runtime.step' @{frames=$warmupChunk}
                    $remainingWarmup-=$warmupChunk
                }
                $passes=@();$offset=0;$snapshot=$null
                do{
                    $request=@{section='passes';offset=$offset;limit=100}
                    if($snapshot){$request.snapshotId=$snapshot}
                    $page=Call-Agent 'render.graph.snapshot' $request
                    $passes+=@($page.items);$offset=$page.nextOffset;$snapshot=$page.snapshotId
                }while($null -ne $offset)
                Write-Json (Join-Path $sceneDirectory ($path+'-passes.json')) $passes
                $names=@($passes|Where-Object{!$_.culled}|ForEach-Object{$_.name})
                if($names -notcontains 'SceneTAA' -or ($case.name -ne 'vfx-baseline' -and $names -notcontains 'PostTaaUnjitteredDepth')){throw 'Required TAA/post-composition depth did not execute.'}
                if($case.vfx -and $names -notcontains 'VFXTransparent'){throw 'Particle fixture has no executing VFX pass.'}
                if($case.name -eq 'distortion' -and $names -notcontains 'VFXBackgroundCopy'){throw 'Selected effect does not require an actual distortion background snapshot.'}
                $observations=@();$lastFrame=-1
                try{
                    foreach($phase in @('fixed','moving')){
                        if($phase -eq 'moving'){$null=Call-Agent 'input.set' @{held=@('w')}}
                        $captureSequence=Call-Agent 'capture.sequence' @{prefix=($path+'-'+$phase);frames=$Frames}
                        if(@($captureSequence.frames).Count -ne $Frames){throw 'Sequence returned an incomplete image set.'}
                        $view=Call-Agent 'render.view.status'
                        for($index=0;$index -lt $Frames;$index++){
                            $capture=$captureSequence.frames[$index]
                            $destination=Join-Path $sceneDirectory ('{0}-{1}-{2:d3}.png' -f $path,$phase,$index)
                            Copy-Item -LiteralPath $capture.path -Destination $destination
                            $capture.path=$destination
                            if([long]$capture.renderFrame -le $lastFrame){throw 'Captured render frames did not advance.'}
                            $lastFrame=[long]$capture.renderFrame
                            if($case.vfx -and ($view.vfx.activeEffectsStatus -ne 1 -or $view.vfx.activeEffects -le 0 -or
                                $view.vfx.drawCallsStatus -ne 1 -or $view.vfx.drawCalls -le 0)){
                                Write-Json (Join-Path $sceneDirectory 'inactive-vfx.json') $view
                                throw 'VFX has no exact live effect/draw submission evidence; this frame cannot cover particles.'
                            }
                            if($case.name -eq 'surface' -and ($view.customGpuDraws -lt 1 -or $view.customGpuObjects -lt 1)){throw 'Compatible custom material has no GPU submission evidence.'}
                            if($case.vfx){
                                $baseline=Join-Path (Join-Path $OutputDirectory 'vfx-baseline') ('{0}-{1}-{2:d3}.png' -f $path,$phase,$index)
                                $differencePath=Join-Path $sceneDirectory ('{0}-{1}-{2:d3}-visibility.json' -f $path,$phase,$index)
                                $null=& $PythonExecutable (Join-Path $PSScriptRoot 'Compare-RenderingImages.py') $baseline $capture.path --output $differencePath
                                if($LASTEXITCODE -ne 0){throw 'Visible particle comparison failed.'}
                                $difference=Get-Content -LiteralPath $differencePath -Raw|ConvertFrom-Json
                                if($difference.maximumChannelDifference -lt 16 -or $difference.channelFractionOver4 -lt 0.0001){throw 'No sufficient visible effect signal versus matching effect-free camera/frame baseline.'}
                            }
                            $observations+=@{phase=$phase;simulationStep=$capture.simulationStep;capture=$capture;view=$view;viewObservation='Sampled after the sequence; not a per-image VFX count.'}
                        }
                    }
                }finally{$null=Call-Agent 'input.clear'}
                $fixed=@($observations|Where-Object{$_.phase -eq 'fixed'})
                $moving=@($observations|Where-Object{$_.phase -eq 'moving'})
                if([Math]::Abs([double]$moving[-1].view.cameraPosition[0]-[double]$fixed[-1].view.cameraPosition[0]) -lt 0.001){throw 'Submitted camera position did not actually move.'}
                Write-Json (Join-Path $sceneDirectory ($path+'-sequence.json')) $observations
                $results.Add(@{case=$case.name;path=$path;status='CAPTURED';visualStatus='NOT_REVIEWED';evidenceType='runtime-manual';framesPerPhase=$Frames;particleMode=$(if($case.name -eq 'distortion' -or $ParticleEffectPath -or $VfxScenario -eq 'cpu20k-proxy'){'CPU effect; no GPU particle coverage claim'}else{'gpu20k requested; inspect GPU metric status'});detail='One simulation step per capture. Camera translation, required passes and visible VFX signal verified; temporal image quality still requires sequence review.'})
          }finally{
            $null=Invoke-HeCtl @('agent','stop') 'stop' $false
          }
        }
    }
    $state='success'
}finally{
    foreach($name in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process')}
    $fixtureArchive=Join-Path $OutputDirectory 'inputs'
    New-Item -ItemType Directory -Path $fixtureArchive -Force | Out-Null
    Get-ChildItem -LiteralPath $fixtureRoot -File | Copy-Item -Destination $fixtureArchive
    if($state -eq 'success'){
        $resolvedFixture=[IO.Path]::GetFullPath($fixtureRoot)
        $allowedFixtureParent=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'Project/Assets/RenderingBenchmarks/TemporalValidation'))+[IO.Path]::DirectorySeparatorChar
        if(!$resolvedFixture.StartsWith($allowedFixtureParent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedFixture) -ne $ownerTag){throw 'Temporal fixture cleanup escaped its owned directory.'}
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
    Write-Json (Join-Path $OutputDirectory 'results.json') $results
    Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
    Write-Json (Join-Path $OutputDirectory 'configuration.json') @{graphicsApi=$GraphicsApi;frames=$Frames;warmupFrames=$WarmupFrames;caseWarmupFrames=$CaseWarmupFrames;fixedDeltaSeconds=1.0/60.0;fixtureRoot=$fixtureRoot;fixtureArchive=$fixtureArchive;distortionEffect=$DistortionEffectPath;particleEffect=$ParticleEffectPath;vfxScenario=$VfxScenario;existingBuildSha256=$ExistingBuildSha256;interpretation='Correctness capture with validation, not performance measurements; view statistics are sampled after each sequence, not individually for each image. customGpuObjects counts submitted candidates, not visible GPU results.'}
    Complete-HEStorageRun -Run $run -State $state
}
