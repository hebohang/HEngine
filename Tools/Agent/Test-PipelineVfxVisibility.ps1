[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='Vulkan',
    [string]$ExistingBuildSha256='',
    [ValidateRange(1,8)][int]$Handles=8,
    [ValidateRange(0.001,10.0)][double]$EffectScale=0.08,
    [ValidateRange(60,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new diagnostic directory.'}
if(Get-Process -Name HEngineEditor,HGame -ErrorAction SilentlyContinue){throw 'Stop existing GPU sessions before running this serial diagnostic.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$ownerTag=[guid]::NewGuid().ToString('N');$sequence=0
$hectl=Join-Path $PSScriptRoot 'hectl.ps1';$SampleMemory=$false;$expectedSize=@(1280,720)
$commands=[Collections.Generic.List[object]]::new();$results=[Collections.Generic.List[object]]::new()
$parseErrors=$null;$tokens=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-RenderingPaths.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Rendering helper source failed parsing.'}
foreach($name in @('Write-Json','Invoke-HeCtl','Call-Agent','Save-Capture','Get-GraphPasses')){
    $definition=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true) | Select-Object -First 1
    if(!$definition){throw "Missing helper: $name"}
    . ([scriptblock]::Create($definition.Extent.Text))
}
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-vfx-visibility' -Paths @($OutputDirectory) -Outer
$state='failed';$savedEnvironment=@{}
$environment=@{
    HE_VFX_STRESS_MODE='1';HE_VFX_STRESS_SCENARIO='cpu20k-proxy';HE_VFX_STRESS_SEED='1806'
    HE_VFX_STRESS_CPU_MAX_HANDLES="$Handles";HE_VFX_STRESS_CPU_EFFECT='';HE_VFX_STRESS_EFFECT=''
    HE_VFX_STRESS_CPU_SCALE=$EffectScale.ToString([Globalization.CultureInfo]::InvariantCulture)
    HE_VFX_STATS_LOG='1';HE_VFX_DISABLE_STATS='0'
}
foreach($name in $environment.Keys){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$currentScene='Assets/Scenes/VFXStressTest.he'
$sampleSteps=@(1,10,20,40,60,120,170)
try {
    foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name],'Process')}
    foreach($case in @(
        @{name='FireBall';effect='Assets/Effects/FireBall.efkpkg'},
        @{name='MagicTornade';effect='Assets/Effects/Survivor/MagicTornade.efkpkg'}
    )) {
        $sceneDirectory=Join-Path $OutputDirectory $case.name
        New-Item -ItemType Directory -Path $sceneDirectory | Out-Null
        [Environment]::SetEnvironmentVariable('HE_VFX_STRESS_CPU_EFFECT',$case.effect,'Process')
        $sessionRoot=$null;$completed=0
        try {
            $arguments=@('agent','start','-StartupScene',$currentScene,'-Resolution','1280x720','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-RenderValidation','-FixedDeltaSeconds','0.0166667')
            if($ExistingBuildSha256){$arguments+=@('-ExistingBuildSha256',$ExistingBuildSha256)}
            $null=Invoke-HeCtl $arguments 'start' $false
            $startLog=Get-Content -LiteralPath $commands[$commands.Count-1].output -Raw
            $sessionMatch=[regex]::Match($startLog,'(?m)^session:\s*(.+?)\s*$')
            if(!$sessionMatch.Success){throw 'Start receipt has no owned session path.'}
            $sessionRoot=Split-Path -Parent $sessionMatch.Groups[1].Value.Trim()
            $runtimeLog=Join-Path $sessionRoot 'game.stdout.log'
            $pidMatch=[regex]::Match($startLog,'agent session: READY pid=(\d+)')
            if(!$pidMatch.Success){throw 'Start receipt has no owned PID.'}
            $process=[Diagnostics.Process]::GetProcessById([int]$pidMatch.Groups[1].Value)
            try {$binaryPath=$process.MainModule.FileName;$binaryHash=(Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash}
            finally {$process.Dispose()}
            Write-Json (Join-Path $sceneDirectory 'binary.json') @{path=$binaryPath;sha256=$binaryHash;requestedExistingSha256=$ExistingBuildSha256}
            # No render.view.create: every capture and graph observation uses main.
            foreach($target in $sampleSteps){
                if($target-$completed -gt 1){$null=Call-Agent 'runtime.step' @{frames=($target-$completed-1)}}
                # Stats log is throttled by wall time and emitted during Update,
                # before Draw. Wait while paused, then take exactly one step.
                Start-Sleep -Milliseconds 5200
                $statsBefore=@(Select-String -LiteralPath $runtimeLog -SimpleMatch '[Effekseer][Stats]').Count
                $step=Call-Agent 'runtime.step' @{frames=1}
                $completed=[int]$step.status.world.runtimeControl.completedSteps
                if($completed -ne $target){throw "Simulation step drift: requested $target, observed $completed"}
                $capture=Save-Capture ('step-{0:d3}' -f $target)
                $statsLines=@(Select-String -LiteralPath $runtimeLog -SimpleMatch '[Effekseer][Stats]')
                $latest=$(if($statsLines.Count -gt $statsBefore){$statsLines[-1].Line}else{$null})
                $metrics=$null
                if($latest -match 'effects=([^ ]+) cpuInstances=([^ ]+).*draws=([^ ]+) vertices=([^ ]+)'){
                    $metrics=@{activeEffects=$Matches[1];cpuInstances=$Matches[2];drawCalls=$Matches[3];vertices=$Matches[4]}
                }
                $record=@{effect=$case.effect;step=$completed;simulationTimeSeconds=$step.status.simulationTimeSeconds
                    capture=$capture;statsLine=$latest;metrics=$metrics;visualReview='pending';visibility='UNKNOWN'
                    metricTiming='CPU instances observed during this simulation update; draws/vertices are runtime-global last recorded main view before this update, normally simulation step N-1. They are not capture-frame counts or visible pixels.'}
                $results.Add($record)
                Write-Json (Join-Path $sceneDirectory ('step-{0:d3}.json' -f $target)) $record
                if(!$metrics){throw "No detailed CPU VFX stats for $($case.name) at step $target; check HE_VFX_DETAILED_STATS in the pinned binary."}
            }
            $passes=Get-GraphPasses
            Write-Json (Join-Path $sceneDirectory 'main-passes.json') $passes
            if(@($passes | ForEach-Object {$_.name}) -notcontains 'VFXTransparent'){throw 'Main graph has no executing VFXTransparent pass.'}
            if(!(Select-String -LiteralPath $runtimeLog -SimpleMatch ('Package loaded: '+$case.effect) -Quiet)){throw 'Requested CPU effect was not confirmed loaded.'}
        } finally {
            try {$null=Invoke-HeCtl @('agent','stop') 'stop' $false}
            finally {
                if($sessionRoot){foreach($file in @('game.stdout.log','game.stderr.log')){
                    $source=Join-Path $sessionRoot $file
                    if(Test-Path -LiteralPath $source){Copy-Item -LiteralPath $source -Destination (Join-Path $sceneDirectory $file)}
                }}
            }
        }
    }
    $state='success'
} finally {
    foreach($name in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process')}
    Write-Json (Join-Path $OutputDirectory 'observations.json') $results
    Write-Json (Join-Path $OutputDirectory 'commands.json') $commands
    Write-Json (Join-Path $OutputDirectory 'manifest.json') @{status=$(if($state -eq 'success'){'DIAGNOSTIC_CAPTURED_VISUAL_PENDING'}else{'FAILED'})
        graphicsApi=$GraphicsApi;existingBuildSha256=$ExistingBuildSha256;handles=$Handles;effectScale=$EffectScale
        sourceScene=$currentScene;sampleSteps=$sampleSteps;views='main only; no preview created';visualReview='pending'
        visibleWindow=$null;interpretation='These sparse samples locate candidate visible times only. Inspect the screenshots, then verify a consecutive-frame sequence within the chosen window. Active handles, submitted draws, and a populated graph do not prove visible particles. FireBall timing changes are diagnostic, not a fix.'}
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "Diagnostic captured at $OutputDirectory; inspect each effect timeline before selecting a continuous-frame window."
