param(
    [string]$Project = 'C:/WorkSpace/HEngineProjects/MechanicGirlLab',
    [ValidateSet('lod-off','balanced','optimized','native','1440')]
    [string[]]$Cases = @('lod-off','balanced','optimized'),
    [ValidateRange(300,18000)][int]$Frames = 1500
)
$ErrorActionPreference = 'Stop'
$engine = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output = Join-Path $Project ('Saved/Diagnostics/TemporalPerformance/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
Get-FileHash (Join-Path $engine 'Engine/Content/Shaders/Modern/SceneTaa.hlsl'), (Join-Path $engine 'Engine/Content/Shaders/Modern/SceneTaaReconstruct.hlsl') | ConvertTo-Json | Set-Content (Join-Path $output 'shader-sources.json') -Encoding utf8
$quality = Get-Content (Join-Path $Project 'Config/RenderQuality.json') -Raw | ConvertFrom-Json
$sourceReplay = Get-Content (Join-Path $Project 'Tools/RenderUpgradeGameplay.replay.json') -Raw | ConvertFrom-Json
# The authored replay ends after 1800 frames. Repeat its gameplay portion so
# longer measurements do not silently become idle-camera benchmarks.
$segments = [System.Collections.Generic.List[object]]::new()
$remaining = $Frames
foreach ($segment in $sourceReplay.segments | Select-Object -First 4) {
    $count = [Math]::Min([int]$segment.frames, $remaining)
    if ($count -gt 0) { $segments.Add(@{frames=$count;held=@($segment.held)}); $remaining -= $count }
}
$gameplay = @($sourceReplay.segments | Select-Object -Skip 4)
if ($gameplay.Count -eq 0) { throw 'Input replay contains no gameplay segments.' }
if (@($sourceReplay.segments | Where-Object { [int]$_.frames -le 0 }).Count -gt 0) { throw 'Input replay contains nonpositive segment lengths.' }
while ($remaining -gt 0) {
    foreach ($segment in $gameplay) {
        $count = [Math]::Min([int]$segment.frames, $remaining)
        if ($count -gt 0) { $segments.Add(@{frames=$count;held=@($segment.held)}); $remaining -= $count }
    }
}
$replayPath = Join-Path $output 'gameplay.replay.json'
@{version=1;segments=@($segments.ToArray())} | ConvertTo-Json -Depth 8 | Set-Content $replayPath -Encoding utf8
foreach ($case in $Cases) {
    $patch = ($quality.Profiles | Where-Object Id -eq 'balanced').Patch | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $height = switch ($case) { 'native' {0}; '1440' {1440}; default {1080} }
    $values = @{
        'Rendering.Presentation.VSync'=$false
        'Rendering.Resolution.RenderHeight'=$height
        'Rendering.LOD.Enabled'=($case -ne 'lod-off')
        'Rendering.AntiAliasing.TaaOptimizedHistoryGather'=($case -eq 'optimized')
        'Rendering.AntiAliasing.TaaSharedNeighborhood'=($case -eq 'optimized')
    }
    foreach ($key in $values.Keys) { $patch | Add-Member -NotePropertyName $key -NotePropertyValue $values[$key] -Force }
    $settings = Join-Path $output ($case + '.json')
    $csv = Join-Path $output ($case + '.csv')
    $patch | ConvertTo-Json -Depth 12 | Set-Content $settings -Encoding utf8
    & (Join-Path $engine 'hectl.bat') game check -Project $Project -UseProjectDisplaySettings -StartupScene Assets/Scenes/SickaDynasty.he -InputReplayPath $replayPath -Frames $Frames -GraphicsApi DX12 -StrictGraphicsApi -FixedDeltaSeconds 0.0166667 -RenderSettingsPath $settings -RenderStats -RenderStatsPath $csv -PerformanceWarmupFrames 120 -TimeoutSeconds 300 *> (Join-Path $output ($case + '.log'))
    if ($LASTEXITCODE -ne 0) { throw "$case runtime failed; see $output" }
    & python (Join-Path $Project 'Tools/Summarize-RenderUpgrade.py') $csv > (Join-Path $output ($case + '.summary.stdout.json'))
    if ($LASTEXITCODE -ne 0) { throw "$case analysis failed" }
    Write-Output "runtime-manual $case PASS: $csv"
}
