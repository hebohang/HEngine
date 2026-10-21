[CmdletBinding()]
param(
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [string]$OutputDirectory = '',
    [ValidateRange(1200,100000)][int]$Frames = 2400,
    [ValidateRange(1,99999)][int]$WarmupFrames = 1200
)
$ErrorActionPreference = 'Stop'
if ($WarmupFrames -ge $Frames) { throw 'Warmup must leave measured frames.' }
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repositoryRoot ('out/agent/ao-performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$cases = [ordered]@{
    previous = @{ 'Rendering.AO.Technique'='GTAO'; 'Rendering.AO.SliceCount'=4;
        'Rendering.AO.StepsPerSide'=8; 'Rendering.AO.FastUpsample'=$false;
        'Rendering.Reflections.SSR.Enabled'=$true }
    reference = @{ 'Rendering.AO.Technique'='GTAO'; 'Rendering.AO.SliceCount'=4;
        'Rendering.AO.StepsPerSide'=8; 'Rendering.AO.FastUpsample'=$false;
        'Rendering.Reflections.SSR.Enabled'=$false }
    current = @{}
    ssao = @{ 'Rendering.AO.Technique'='SSAO'; 'Rendering.AO.SampleCount'=16;
        'Rendering.AO.FastUpsample'=$true; 'Rendering.Reflections.SSR.Enabled'=$false }
    currentRepeat = @{}
}
$commands = [Collections.Generic.List[object]]::new()
$oldLocation = Get-Location
try {
    Set-Location $repositoryRoot
    foreach ($case in $cases.GetEnumerator()) {
        $prefix = Join-Path $OutputDirectory $case.Key
        [IO.File]::WriteAllText($prefix+'.json',($case.Value | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        # 0.001 s per simulation step deliberately keeps this diagnostic in the
        # initial gameplay camera, before the blocking level-up menu. Rendering
        # stays native 4K, once per frame. This is not real-time gameplay CPU or
        # long-duration thermal-stability acceptance; record that distinction.
        $arguments = @('game','capture','-StartupScene','Assets/Scenes/MainGame.he',
            '-GraphicsApi',$GraphicsApi,'-StrictGraphicsApi','-Frames',"$Frames",
            '-PerformanceWarmupFrames',"$WarmupFrames",'-FixedDeltaSeconds','0.001',
            '-Resolution','3840x2160','-RenderStats','-RenderStatsPath',($prefix+'.csv'),
            '-RenderSettingsPath',($prefix+'.json'),'-CapturePath',($prefix+'.png'),'-TimeoutSeconds','300')
        & (Join-Path $repositoryRoot 'hectl.bat') @arguments *> ($prefix+'.log')
        $code = $LASTEXITCODE
        $commands.Add(@{ program='hectl.bat'; arguments=$arguments; exitCode=$code; log=$prefix+'.log' })
        if ($code -ne 0) { throw "AO performance case $($case.Key) failed; see $prefix.log" }
        Write-Output "$($case.Key): PASS"
    }
} finally {
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'commands.json'),
        ($commands | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    Set-Location $oldLocation
}
