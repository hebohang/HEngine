[CmdletBinding()]
param(
    [string]$RepositoryRoot=([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))),
    [string]$OutputDirectory='',
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExistingBuildSha256,
    [string]$PythonExecutable='python',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath($RepositoryRoot)
$agentRoot=Join-Path $repositoryRoot 'Tools/Agent'
$hectl=Join-Path $agentRoot 'hectl.ps1'
if(!$OutputDirectory){$OutputDirectory=Join-Path $repositoryRoot ('out/agent/pipeline-upgrade/vulkan-cache-retirement/run-'+(Get-Date -Format yyyyMMdd-HHmmss))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N'); $sequence=0; $sceneDirectory=$OutputDirectory
$SampleMemory=$false; $expectedSize=@(1280,720)
$commands=[Collections.Generic.List[object]]::new(); $results=[Collections.Generic.List[object]]::new()
# Import definitions only; never execute another driver's top-level statements.
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $agentRoot 'Test-PipelineCaptureSequence.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Shared capture helper parse failed.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Save-Capture','Get-GraphPasses','Send-MailboxBatch','Save-Sequence')){
    $definitions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false)|Where-Object Name -eq $name)
    if($definitions.Count -ne 1){throw "Missing unique shared helper $name"}
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}
. (Join-Path $agentRoot 'Storage.ps1')
$fixtureRelative='Assets/AgentCacheRetirement/'+$ownerTag
$fixtureRoot=Join-Path $repositoryRoot ('Project/'+$fixtureRelative)
$fixtureModule='AgentVfxCache_'+$ownerTag
$fixtureScriptPath=Join-Path $repositoryRoot ('Project/Scripts/'+$fixtureModule+'.py')
if(Test-Path -LiteralPath $fixtureScriptPath){throw 'Unique Python fixture already exists.'}
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$storage=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'vulkan-cache-retirement' -Paths @($OutputDirectory,$fixtureRoot,$fixtureScriptPath) -Outer
$state='failed'; $ownedSession=$null; $ownedProcess=$null; $started=$false
$environment=@{HE_VFX_STRESS_MODE='1';HE_VFX_STRESS_SCENARIO='cpu20k-proxy';HE_VFX_STRESS_SEED='1806';HE_VFX_STRESS_CPU_MAX_HANDLES='8';HE_VFX_STRESS_CPU_EFFECT='Assets/Effects/Survivor/MagicTornade.efkpkg';HE_VFX_STRESS_CPU_SCALE='0.08';HE_VFX_STRESS_EFFECT='';HE_VFX_STATS_LOG='1';HE_VFX_DISABLE_STATS='0';HE_VFX_CACHE_RETIREMENT_LOG='1'}
$savedEnvironment=@{}
foreach($key in $environment.Keys){$savedEnvironment[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$baselines=@{}; $cycles=[Collections.Generic.List[object]]::new()
function Read-CacheRows {
    $path=Join-Path ([string]$ownedSession.runDirectory) 'game.stdout.log'
    $pattern='\[Effekseer\]\[VulkanCacheRetirement\] generation=(\d+) completed=(\d+) removedFramebuffers=(\d+) remainingFramebuffers=(\d+) removedTextures=(\d+) remainingTextures=(\d+) ownersOnly=1'
    $rows=@()
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream)
    try{
        while($null -ne ($line=$reader.ReadLine())){
            if($line -match $pattern){$rows+=[pscustomobject]@{generation=[long]$Matches[1];completed=[long]$Matches[2];removedFramebuffers=[long]$Matches[3];remainingFramebuffers=[long]$Matches[4];removedTextures=[long]$Matches[5];remainingTextures=[long]$Matches[6]}}
        }
    }finally{$reader.Dispose()}
    return $rows
}
function Wait-Steps {
    $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do{
        $status=Call-Agent 'session.status'
        $control=$status.world.runtimeControl
        if($null -eq $control -or $null -eq $control.pendingSteps -or $null -eq $control.stepInProgress){throw 'Session status has no runtime step completion state.'}
        if([long]$control.pendingSteps -eq 0 -and !$control.stepInProgress -and $control.controlMode -eq 'paused'){return}
        if([DateTime]::UtcNow -ge $deadline){throw 'Timed out waiting for paused simulation with no queued/in-progress steps.'}
        Start-Sleep -Milliseconds 100
    }while($true)
}
function Capture-Eight([string]$label,[int[]]$size){
    Wait-Steps
    $capture=Call-Agent 'capture.sequence' @{prefix=$label;frames=8}
    Save-Sequence $capture $label
    $index=0; $images=@()
    foreach($frame in $capture.frames){$images+=@(Save-Capture ($label+('-{0:d2}' -f $index)) $size $frame);$index++}
    return @{capture=$capture;images=$images}
}
function Capture-Preview([string]$label,[int[]]$size){
    $null=Call-Agent 'render.view.capture'
    for($attempt=0;$attempt -lt 40;$attempt++){
        $null=Call-Agent 'runtime.step' @{frames=2}
        Wait-Steps
        $view=Call-Agent 'render.view.status'
        if($view.capture.status -eq 'failed'){throw 'Preview readback failed.'}
        if($view.capture.status -eq 'complete'){break}
    }
    if(!$view.available -or !$view.hasRecordedGraph -or $view.capture.status -ne 'complete'){throw 'Preview did not produce a graph and readback.'}
    Write-Json (Join-Path $sceneDirectory ($label+'-status.json')) $view
    $image=Save-Capture $label $size $view.capture
    # The Agent's per-view filename counter resets on destroy/create. Preserve
    # the completed original outside its mailbox before reusing that counter.
    $source=[IO.Path]::GetFullPath([string]$view.capture.path)
    $captureRoot=[IO.Path]::GetFullPath((Join-Path ([string]$ownedSession.sessionDirectory) 'captures'))
    if($ownedSession.ownerTag -ne $ownerTag -or
        ![string]::Equals([IO.Path]::GetDirectoryName($source),$captureRoot,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($source) -notmatch '^offscreen-\d+\.png$'){
        throw 'Completed preview path is outside this owned mailbox.'
    }
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $image.sha256){throw 'Preview archive hash mismatch.'}
    $originalArchive=Join-Path $sceneDirectory ($label+'-original.png')
    if(Test-Path -LiteralPath $originalArchive){throw 'Preview original archive already exists.'}
    Move-Item -LiteralPath $source -Destination $originalArchive
    return @{image=$image;view=$view}
}
function Assert-Signal([string]$reference,[string]$candidate,[string]$label){
    $path=Join-Path $sceneDirectory ($label+'-difference.json')
    $null=& $PythonExecutable (Join-Path $agentRoot 'Compare-RenderingImages.py') $reference $candidate --output $path
    if($LASTEXITCODE -ne 0){throw 'Image difference helper failed.'}
    $difference=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if($difference.maximumChannelDifference -lt 16 -or $difference.channelFractionOver4 -lt 0.0001){throw "No visible effect signal relative to effect-free fixture: $label"}
}
function Stop-Owned {
    if(!$started){return}
    try{Invoke-HeCtl @('agent','stop') 'stop' $false}
    finally{
        if($ownedSession){foreach($name in @('game.stdout.log','game.stderr.log')){
            $path=Join-Path ([string]$ownedSession.runDirectory) $name
            if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $sceneDirectory $name)}
        }}
        $script:started=$false
        if($ownedProcess){$ownedProcess.Dispose();$script:ownedProcess=$null}
    }
}
try{
    $scriptSource=Join-Path $repositoryRoot 'Project/Scripts/VFXStressTest.py'
    $pythonFixture=[IO.File]::ReadAllText($scriptSource).Replace("`r`n","`n")
    $speedArgument="            scale,`n            1.0,"
    if([regex]::Matches($pythonFixture,'class VFXStressTest:').Count -ne 1 -or
        [regex]::Matches($pythonFixture,[regex]::Escape($speedArgument)).Count -ne 1 -or
        [regex]::Matches($pythonFixture,'HEngine\.VFX\.PlayEffectAdvanced\(').Count -ne 1){throw 'VFX source changed; review the bounded speed fixture transformation.'}
    # Static recursive helpers also reference the class by name. Rename every
    # complete identifier, not just the declaration, in this isolated module.
    $pythonFixture=[regex]::Replace($pythonFixture,'\bVFXStressTest\b',$fixtureModule)
    $pythonFixture=$pythonFixture.Replace($speedArgument,"            scale,`n            0.1,")
    $probeStart=@'
    def Start(self, entity):
        self.cache_probe_handle = -1
        if not HEngine.VFX.LoadEffect("Assets/Effects/FireBall.efkpkg"):
            raise RuntimeError("Cache retirement FireBall probe failed to load")
        self.cache_probe_handle = HEngine.VFX.PlayEffectAdvanced(
            "Assets/Effects/FireBall.efkpkg", 0.0, 0.65, 0.0,
            0.0, 1.0, 0.0, 0.08, 0.01)
        if self.cache_probe_handle < 0:
            raise RuntimeError("Cache retirement FireBall probe failed to start")
        HEngine.HE_CORE_INFO("[CacheRetirementFixture] FireBall snapshot probe speed=0.01; MagicTornade visibility speed=0.1")
'@
    $probeStop=@'
    def Stop(self):
        probe = getattr(self, "cache_probe_handle", -1)
        if probe >= 0 and HEngine.VFX.IsEffectPlaying(probe):
            HEngine.VFX.StopEffect(probe)
        self.cache_probe_handle = -1
'@
    if([regex]::Matches($pythonFixture,'    def Start\(self, entity\):').Count -ne 1 -or
        [regex]::Matches($pythonFixture,'    def Stop\(self\):').Count -ne 1){throw 'VFX lifecycle changed; review probe injection.'}
    $pythonFixture=$pythonFixture.Replace('    def Start(self, entity):',$probeStart.Replace("`r`n","`n")).Replace('    def Stop(self):',$probeStop.Replace("`r`n","`n"))
    [IO.File]::WriteAllText($fixtureScriptPath,$pythonFixture,[Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath $fixtureScriptPath -Destination (Join-Path $OutputDirectory ($fixtureModule+'.py'))
    $preflight=@'
import json
import runpy
import sys
import types

# Execute the actual generated module without importing the engine or GPU.
sys.modules["HEngine"] = types.ModuleType("HEngine")
namespace = runpy.run_path(sys.argv[1])
fixture_class = namespace[sys.argv[2]]
value = {"x": [1, {"y": (True, None)}]}
expected = {"x": [1, {"y": [True, None]}]}
assert fixture_class._plain(value) == expected
assert json.loads(fixture_class._encode(value)) == expected
assert json.loads(fixture_class._quote('line\n"quoted"')) == 'line\n"quoted"'
instance = fixture_class()
assert instance.handles == []
print("PASS: generated module executes; recursive _plain/_encode, _quote and constructor verified with stub HEngine")
'@
    $preflightPath=Join-Path $OutputDirectory 'python-fixture-preflight.py'
    [IO.File]::WriteAllText($preflightPath,$preflight,[Text.UTF8Encoding]::new($false))
    & $PythonExecutable $preflightPath $fixtureScriptPath $fixtureModule *> (Join-Path $OutputDirectory 'python-fixture-preflight.log')
    if($LASTEXITCODE -ne 0){throw 'Generated Python fixture execution failed; see python-fixture-preflight.log.'}
    Write-Json (Join-Path $OutputDirectory 'python-fixture.json') @{source=$scriptSource;sourceSha256=(Get-FileHash -LiteralPath $scriptSource).Hash;module=('Scripts.'+$fixtureModule+'.py');fixtureSha256=(Get-FileHash -LiteralPath $fixtureScriptPath).Hash;effectSpeed=0.1;probe=@{effect='Assets/Effects/FireBall.efkpkg';speed=0.01;handles=1;purpose='Keep actual background snapshot imports active; soft-depth wrapper is not covered by this fixture. Does not establish FireBall visibility or refraction quality.'};warmupSteps=300;purpose='Slow CPU fixture validates native framebuffer and external snapshot Owner retirement. Full-speed temporal behavior is validated separately; no scene reload occurs between cycles.'}
    foreach($key in $environment.Keys){[Environment]::SetEnvironmentVariable($key,$environment[$key],'Process')}
    foreach($mode in @('baseline','particles')){
        $sceneDirectory=Join-Path $OutputDirectory $mode
        New-Item -ItemType Directory -Path $sceneDirectory | Out-Null
        $scene=Get-Content -LiteralPath (Join-Path $repositoryRoot 'Project/Assets/Scenes/VFXStressTest.he') -Raw|ConvertFrom-Json
        $cameras=0;$directors=0
        foreach($entity in $scene.PSObject.Properties){
            if($mode -eq 'baseline'){$entity.Value.PSObject.Properties.Remove('PythonScriptComponent')}
            elseif($entity.Value.PythonScriptComponent.Path -eq 'Scripts.VFXStressTest.py'){$entity.Value.PythonScriptComponent.Path='Scripts.'+$fixtureModule+'.py';$directors++}
            if($entity.Value.CameraComponent){$entity.Value|Add-Member -NotePropertyName PythonScriptComponent -NotePropertyValue @{Path='Scripts.PBRShadowCaptureCamera.py'} -Force;$cameras++}
        }
        if($cameras -ne 1){throw 'Expected one fixed fixture camera.'}
        if($mode -eq 'particles' -and $directors -ne 1){throw 'Expected one isolated slow-speed VFX director.'}
        Write-Json (Join-Path $fixtureRoot ($mode+'.he')) $scene
        Copy-Item -LiteralPath (Join-Path $fixtureRoot ($mode+'.he')) -Destination (Join-Path $sceneDirectory 'fixture.he')
        $currentScene=$fixtureRelative+'/'+$mode+'.he'
        $started=$true
        Invoke-HeCtl @('agent','start','-GraphicsApi','Vulkan','-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667','-Resolution','1280x720','-StartupScene',$currentScene,'-ExistingBuildSha256',$ExistingBuildSha256) 'start' $false
        $ownedSession=Get-Content -LiteralPath (Join-Path $repositoryRoot 'out/agent/sessions/active.json') -Raw|ConvertFrom-Json
        if($ownedSession.ownerTag -ne $ownerTag){throw 'Session owner mismatch.'}
        $ownedProcess=Get-Process -Id ([int]$ownedSession.pid)
        if($ownedProcess.StartTime.ToUniversalTime().Ticks -ne [long]$ownedSession.processStartUtcTicks){throw 'Process identity mismatch.'}
        $binary=$ownedProcess.MainModule.FileName
        Write-Json (Join-Path $sceneDirectory 'binary.json') @{path=$binary;sha256=(Get-FileHash -LiteralPath $binary).Hash;requestedSha256=$ExistingBuildSha256}
        $settings=Call-Agent 'render.settings.get'
        $receipt=Call-Agent 'render.settings.apply' @{layer='Session';baseRevision=$settings.revision;baseHash=$settings.hash;persist=$false;patch=@{'Rendering.Path'='Forward';'Rendering.AntiAliasing.Mode'='FXAA';'Rendering.GI.Technique'='Off';'Rendering.GI.Denoiser'='None';'Rendering.Clouds.Quality'='Off';'Rendering.PostProcess.AutoExposure.Enabled'=$false;'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.Views.Overrides'=@{preview=@{'Rendering.Path'='Deferred'}}}}
        if(!$receipt.success){throw 'Session settings rejected.'}
        $settings=Call-Agent 'render.settings.get'
        Write-Json (Join-Path $sceneDirectory 'settings.json') $settings
        $fif=[int]$settings.effective.'Rendering.Presentation.FramesInFlight'
        if($fif -lt 1 -or 8 -lt $fif+2){throw 'Eight frames do not cover FramesInFlight + 2.'}
        foreach($warmupChunk in @(120,120,60)){
            $null=Call-Agent 'runtime.step' @{frames=$warmupChunk}
            Wait-Steps
        }
        $count=$(if($mode -eq 'baseline'){2}else{24})
        $previousGeneration=$null
        for($cycle=0;$cycle -lt $count;$cycle++){
            $size=$(if($cycle%2 -eq 0){@(640,360)}else{@(1280,720)})
            $label=('{0}-{1:d2}' -f $mode,$cycle)
            $beforeRows=@(Read-CacheRows).Count
            if($cycle -gt 0){$destroy=Call-Agent 'render.view.destroy';if($destroy.available){throw 'Preview destroy did not remove view.'}}
            $resize=Call-Agent 'window.resize' @{width=$size[0];height=$size[1]}
            if($resize.clientWidth -ne $size[0] -or $resize.clientHeight -ne $size[1]){throw 'Client resize failed.'}
            $null=Call-Agent 'render.view.create' @{width=$size[0];height=$size[1]}
            $main=Capture-Eight $label $size
            $preview=Capture-Preview ($label+'-preview') $size
            if($null -ne $previousGeneration -and $preview.view.viewGeneration -le $previousGeneration){throw 'Preview reconstruction generation did not increase.'}
            $previousGeneration=$preview.view.viewGeneration
            $status=Call-Agent 'session.status'
            if($status.renderWidth -ne $size[0] -or $status.renderHeight -ne $size[1]){throw 'Main rendering did not resize.'}
            $passes=@(Get-GraphPasses)
            Write-Json (Join-Path $sceneDirectory ($label+'-passes.json')) $passes
            $resources=@();$offset=0;$snapshot=''
            do{
                $parameters=@{section='resources';offset=$offset;limit=100}
                if($snapshot){$parameters.snapshotId=$snapshot}
                $page=Call-Agent 'render.graph.snapshot' $parameters
                $resources+=@($page.items);$offset=$page.nextOffset;$snapshot=[string]$page.snapshotId
            }while($null -ne $offset)
            Write-Json (Join-Path $sceneDirectory ($label+'-resources.json')) $resources
            Write-Json (Join-Path $sceneDirectory ($label+'-status.json')) $status
            if($mode -eq 'baseline'){$baselines[$size[0]]=@{main=$main.images[-1].path;preview=$preview.image.path}}
            else{
                if(@($passes.name) -notcontains 'VFXTransparent'){throw 'Main VFX pass absent.'}
                if($preview.view.passes -notcontains 'VFXTransparent' -or $preview.view.vfx.drawCallsStatus -ne 1 -or $preview.view.vfx.drawCalls -le 0){throw 'Preview VFX graph/global last-view draw evidence absent.'}
                foreach($requiredSnapshot in @('VFXBackgroundCopy')){
                    if(@($passes.name) -notcontains $requiredSnapshot -or $preview.view.passes -notcontains $requiredSnapshot){throw "Both views must execute the FireBall external snapshot probe: $requiredSnapshot"}
                }
                Assert-Signal $baselines[$size[0]].main $main.images[-1].path ($label+'-main')
                Assert-Signal $baselines[$size[0]].preview $preview.image.path ($label+'-preview')
                $rows=@(Read-CacheRows);$newRows=@($rows|Select-Object -Skip $beforeRows)
                if($cycle -ge 2 -and $newRows.Count -eq 0){throw 'No old cache Owner retirement after resize and eight submitted frames.'}
                $ownedProcess.Refresh()
                $cycles.Add(@{cycle=$cycle;size=$size;cacheRetirements=$newRows;privateBytes=$ownedProcess.PrivateMemorySize64;workingSetBytes=$ownedProcess.WorkingSet64;renderFrames=@($main.capture.frames.renderFrame);previewGeneration=$previousGeneration})
            }
        }
        if($mode -eq 'particles'){
            Wait-Steps
            $replies=@(Send-MailboxBatch @(@{method='capture.sequence';params=@{prefix='cancel-active';frames=16}},@{method='capture.cancel';params=@{}}) 'cancel-active')
            if(!$replies[0].ok -or !$replies[1].ok -or !$replies[1].result.pending -or $replies[1].result.latest.ticket -ne $replies[0].result.ticket){throw 'Cancellation did not target a live capture.'}
            $status=Call-Agent 'session.status'
            if($status.capture.latest.state -ne 'failed' -or $status.world.runtimeControl.pendingSteps -ne 0){throw 'Cancelled capture did not settle.'}
            $null=Capture-Eight 'quiescence' @(1280,720)
            $null=Capture-Eight 'final-consecutive' @(1280,720)
            $rows=@(Read-CacheRows)
            Write-Json (Join-Path $sceneDirectory 'cache-retirements.json') $rows
            if(($rows|Measure-Object removedFramebuffers -Sum).Sum -le 0 -or ($rows|Measure-Object removedTextures -Sum).Sum -le 0){throw 'Both cache Owner types must actually retire.'}
            foreach($field in @('remainingFramebuffers','remainingTextures')){
                $middle=@($cycles|Where-Object {$_.cycle -ge 8 -and $_.cycle -lt 16}|ForEach-Object {$_.cacheRetirements})
                $late=@($cycles|Where-Object {$_.cycle -ge 16}|ForEach-Object {$_.cacheRetirements})
                $middleMax=($middle|Measure-Object $field -Maximum).Maximum
                $lateMax=($late|Measure-Object $field -Maximum).Maximum
                if($lateMax -gt $middleMax){throw "Two-size Owner working set still grows: $field middle=$middleMax late=$lateMax"}
            }
        }
        Stop-Owned
        $log=Get-Content -LiteralPath (Join-Path $sceneDirectory 'game.stdout.log') -Raw
        $stderr=Get-Content -LiteralPath (Join-Path $sceneDirectory 'game.stderr.log') -Raw
        if(($log+ $stderr) -match 'VUID-|Validation Error|VK_ERROR_DEVICE_LOST|ObjectTracker.*(?:leak|not.*destroy)|live objects'){throw 'Vulkan validation/shutdown failure; inspect owned logs.'}
        $ownedSession=$null
    }
    $state='success'
}finally{
    try{Stop-Owned}finally{
        foreach($key in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($key,$savedEnvironment[$key],'Process')}
        Write-Json (Join-Path $OutputDirectory 'commands.json') @($commands.ToArray())
        Write-Json (Join-Path $OutputDirectory 'results.json') @{state=$state;evidenceType='runtime-manual';cycles=@($cycles.ToArray());visualReview='pending';scope='Cache Owner retirement and two-size count plateau; not LLGI references or physical GPU VRAM. Image difference is visible signal, not visual quality.'}
        # Remove only this run's two known temporary scene files; never recurse.
        foreach($mode in @('baseline','particles')){$file=Join-Path $fixtureRoot ($mode+'.he');if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file}}
        if(Test-Path -LiteralPath $fixtureScriptPath){Remove-Item -LiteralPath $fixtureScriptPath}
        $pythonCache=Join-Path $repositoryRoot 'Project/Scripts/__pycache__'
        if(Test-Path -LiteralPath $pythonCache){
            foreach($bytecode in @(Get-ChildItem -LiteralPath $pythonCache -File -Filter ($fixtureModule+'.*.pyc'))){Remove-Item -LiteralPath $bytecode.FullName}
        }
        if((Test-Path -LiteralPath $fixtureRoot) -and @(Get-ChildItem -LiteralPath $fixtureRoot -Force).Count -eq 0){Remove-Item -LiteralPath $fixtureRoot}
        Complete-HEStorageRun -Run $storage -State $state | Out-Null
    }
}
