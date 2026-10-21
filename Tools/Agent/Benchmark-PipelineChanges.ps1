[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$OutputDirectory,[int]$WarmupFrames=600,[int]$MeasureFrames=1800,[int]$Repeats=3)
$ErrorActionPreference='Stop'
$repositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run=Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'pipeline-ab' -Paths @($OutputDirectory) -Outer
$state='failed'
$oldFull=[Environment]::GetEnvironmentVariable('HE_RENDER_FULL_SURFACE_DATA','Process')
$oldCache=[Environment]::GetEnvironmentVariable('HE_RENDER_DISABLE_MATERIAL_PACK_CACHE','Process')
$records=[Collections.Generic.List[object]]::new()
$settings=@{'Rendering.Path'='Forward';'Rendering.AntiAliasing.Mode'='TAA';'Rendering.GI.Technique'='Off';'Rendering.AO.Enabled'=$true;'Rendering.Reflections.SSR.Enabled'=$false;'Rendering.Reflections.Probes.Enabled'=$false;'Rendering.GpuBatching.Enabled'=$true;'Rendering.GpuBatching.ShadowsEnabled'=$true;'Rendering.Memory.TransientTextureReuse'=$true;'Rendering.Presentation.VSync'=$false;'Rendering.Debug.RenderGraph.Validation'=$false}
$settingsPath=Join-Path $OutputDirectory 'settings.json'
[IO.File]::WriteAllText($settingsPath,($settings|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
try {
    & nvidia-smi --query-gpu=name,driver_version,memory.total,temperature.gpu,power.draw,clocks.sm --format=csv > (Join-Path $OutputDirectory 'hardware-before.csv')
    Get-CimInstance Win32_Processor | Select-Object Name | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'cpu.json')
    & powercfg /getactivescheme > (Join-Path $OutputDirectory 'power-scheme.txt')
    for($repeat=0;$repeat -lt $Repeats;$repeat++) {
        $cases=@(
            @{name='guides-full';scene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he';full='1';cache='0'},
            @{name='guides-compact';scene='Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he';full='0';cache='0'},
            @{name='guides-sponza-full';scene='Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD/IntelSponzaDaylight.he';full='1';cache='0'},
            @{name='guides-sponza-compact';scene='Assets/RenderingBenchmarks/IntelSponza2048BCGPULOD/IntelSponzaDaylight.he';full='0';cache='0'},
            @{name='packing-off';scene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he';full='0';cache='1'},
            @{name='packing-on';scene='Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he';full='0';cache='0'})
        if($repeat % 2){[array]::Reverse($cases)}
        foreach($case in $cases){
            $name=$case.name+'-'+$repeat
            $prefix=Join-Path $OutputDirectory $name
            [Environment]::SetEnvironmentVariable('HE_RENDER_FULL_SURFACE_DATA',$case.full,'Process')
            [Environment]::SetEnvironmentVariable('HE_RENDER_DISABLE_MATERIAL_PACK_CACHE',$case.cache,'Process')
            $scene=$case.scene
            $arguments=@('game','capture','-GraphicsApi','DX12','-StrictGraphicsApi','-FixedDeltaSeconds','0.0166667','-Resolution','1920x1080','-StartupScene',$scene,'-RenderSettingsPath',$settingsPath,'-RenderStats','-RenderStatsPath',($prefix+'.csv'),'-PerformanceWarmupFrames',"$WarmupFrames",'-Frames',"$($WarmupFrames+$MeasureFrames)",'-CapturePath',($prefix+'.png'),'-TimeoutSeconds','300')
            & (Join-Path $repositoryRoot 'hectl.bat') @arguments *> ($prefix+'.log')
            $code=$LASTEXITCODE
            $records.Add(@{name=$name;case=$case.name;repeat=$repeat;scene=$scene;sceneSha256=(Get-FileHash (Join-Path $repositoryRoot ('Project/'+$scene)) -Algorithm SHA256).Hash;arguments=$arguments;exitCode=$code;fullSurface=$case.full;disablePackingCache=$case.cache;warmupFrames=$WarmupFrames;measureFrames=$MeasureFrames;binarySha256=(Get-FileHash (Join-Path $repositoryRoot 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe') -Algorithm SHA256).Hash})
            if($code -ne 0){throw "Benchmark $name failed; see its log."}
        }
    }
    $state='success'
} finally {
    [Environment]::SetEnvironmentVariable('HE_RENDER_FULL_SURFACE_DATA',$oldFull,'Process')
    [Environment]::SetEnvironmentVariable('HE_RENDER_DISABLE_MATERIAL_PACK_CACHE',$oldCache,'Process')
    & nvidia-smi --query-gpu=name,driver_version,memory.total,temperature.gpu,power.draw,clocks.sm --format=csv > (Join-Path $OutputDirectory 'hardware-after.csv')
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'),($records|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "Pipeline A/B sampling $state; evidence: $OutputDirectory"
