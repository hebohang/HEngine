[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi='DX12',
    [ValidatePattern('^$|^[0-9a-fA-F]{64}$')][string]$ExistingBuildSha256='',
    [int]$TimeoutSeconds=180
)
function Get-SsgiSettingsMismatches([hashtable]$Requested, $Evidence) {
    foreach($key in ($Requested.Keys | Sort-Object)) {
        $section=if($key.StartsWith('Rendering.Debug.')){'debug'}else{'effective'}
        $container=if($null -ne $Evidence){$Evidence.$section}else{$null}
        $property=if($null -ne $container){$container.PSObject.Properties[$key]}else{$null}
        $expected=$Requested[$key]
        $actual=if($null -ne $property){$property.Value}else{$null}
        $reason=$null
        if($null -eq $property){$reason='missing field'}
        elseif($null -eq $actual){$reason='null value'}
        elseif($expected -is [bool]){
            if($actual -isnot [bool] -or $actual -ne $expected){$reason='boolean mismatch'}
        }elseif($expected -is [double] -or $expected -is [int] -or $expected -is [long]){
            if($actual -isnot [ValueType] -or $actual -is [bool]){$reason='numeric type mismatch'}
            else{
                # The settings service stores float32; keep only that rounding tolerance.
                $value=[double]$actual
                $tolerance=if($expected -is [double]){0.00001}else{0.0}
                if([double]::IsNaN($value) -or [double]::IsInfinity($value) -or
                    [Math]::Abs($value-[double]$expected) -gt $tolerance){$reason='numeric mismatch'}
            }
        }elseif($actual -isnot [string] -or $actual -cne [string]$expected){$reason='string mismatch'}
        if($reason){[pscustomobject]@{field=$key;section=$section;expected=$expected;actual=$actual;reason=$reason}}
    }
}
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'ssgi-convergence' -Paths @($OutputDirectory) -Outer
$state='failed';$records=[Collections.Generic.List[object]]::new()
$scene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he'
$binary=Join-Path $repositoryRoot 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
try {
    foreach($rays in @(1,8)){
        $settings=@{'Rendering.Path'='Forward';'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='ScreenSpaceGI';
            'Rendering.GI.Denoiser'='SVGF';'Rendering.GI.RenderScale'=0.5;'Rendering.GI.AtrousIterations'=2;
            'Rendering.GI.BilateralUpsample'=$true;'Rendering.GI.SSGI.RayCount'=$rays;'Rendering.GI.SSGI.TemporalWeight'=0.97;
            'Rendering.GI.SSGI.StepCount'=64;'Rendering.GI.SSGI.MaximumRayDistance'=10.0;
            'Rendering.GI.SSGI.Thickness'=0.1;'Rendering.GI.SSGI.BinaryRefinementSteps'=3;'Rendering.GI.SSGI.HiZStartMip'=2;
            'Rendering.GI.SVGF.OptimizedKernels'=$true;'Rendering.GI.MaxBounces'=1;
            'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;
            'Rendering.Presentation.VSync'=$false;'Rendering.Debug.GI.View'=0;'Rendering.Debug.GI.DirectOnly'=$false}
        $settingsPath=Join-Path $OutputDirectory ('rays-'+$rays+'-requested.json')
        [IO.File]::WriteAllText($settingsPath,($settings|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        foreach($frames in @(30,128,512)){
            $name='rays-'+$rays+'-frames-'+$frames;$prefix=Join-Path $OutputDirectory $name
            $beforeHash=if(Test-Path $binary){(Get-FileHash $binary -Algorithm SHA256).Hash}else{''}
            if($ExistingBuildSha256 -and $beforeHash -ne $ExistingBuildSha256){throw 'Existing binary hash does not match before capture.'}
            $arguments=@('game','capture','-Configuration','RelWithDebInfo','-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi',
                '-StartupScene',$scene,'-Resolution','1280x720','-FixedDeltaSeconds','0.0166667',
                '-Frames',"$frames",'-PerformanceWarmupFrames','0','-RenderStats','-RenderStatsPath',($prefix+'.csv'),
                '-RenderSettingsPath',$settingsPath,'-CapturePath',($prefix+'.png'),'-TimeoutSeconds',"$TimeoutSeconds")
            # The normal bounded capture route owns, waits and reaps the process.
            # ExistingBuildSha256 is an audit gate here, not agent-start's no-build mode.
            & (Join-Path $repositoryRoot 'hectl.bat') @arguments *> ($prefix+'.log')
            $code=$LASTEXITCODE
            $afterHash=if(Test-Path $binary){(Get-FileHash $binary -Algorithm SHA256).Hash}else{''}
            $rows=if(Test-Path ($prefix+'.csv')){@(Import-Csv -LiteralPath ($prefix+'.csv'))}else{@()}
            $effectivePath=$prefix+'.csv.settings.json'
            $effective=if(Test-Path $effectivePath){Get-Content -LiteralPath $effectivePath -Raw|ConvertFrom-Json}else{$null}
            $mismatches=@(Get-SsgiSettingsMismatches $settings $effective)
            $matches=$mismatches.Count -eq 0
            [IO.File]::WriteAllText(($prefix+'-settings-check.json'),(@{matches=$matches;mismatches=$mismatches}|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
            $records.Add(@{name=$name;requestedRayCount=$rays;requestedWarmupFrames=0;requestedCaptureAndExitFrame=$frames;
                scene=$scene;sceneSha256=(Get-FileHash (Join-Path $repositoryRoot ('Project/'+$scene)) -Algorithm SHA256).Hash;
                binaryBeforeSha256=$beforeHash;binarySha256=$afterHash;exitCode=$code;arguments=$arguments;
                cpuRows=$rows.Count;actualCsvFrames=@($rows|ForEach-Object{[long]$_.frame});
                actualGpuSamples=@($rows|ForEach-Object{[long]$_.gpuSample}|Sort-Object -Unique);
                gpuWarmupCutoffSamples=@($rows|ForEach-Object{[long]$_.gpuWarmupCutoffSample}|Sort-Object -Unique);
                effectiveSettingsPath=$effectivePath;requestedSettingsObserved=$matches;settingsMismatches=$mismatches;
                passCsvAvailable=(Test-Path ($prefix+'.csv.passes.csv'));imageAvailable=(Test-Path ($prefix+'.png'));
                visualStatus='NOT_REVIEWED';historyPolicy='Fresh process and renderer per endpoint; no runtime.step or shell-delay accumulation. CSV frame is automation completed frame; GPU samples are delayed timestamps, not history age. Actual per-pixel accepted history length is not exposed.'})
            if($code -ne 0){throw "Capture $name failed."}
            if($ExistingBuildSha256 -and $afterHash -ne $ExistingBuildSha256){throw 'Build changed the audited binary; do not accept these measurements as the pinned version.'}
            if(!$matches){throw ('Settings evidence differs from requested experiment: '+($mismatches|ConvertTo-Json -Depth 6 -Compress))}
        }
    }
    $state='success'
}finally{
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
    Complete-HEStorageRun -Run $run -State $state
}
