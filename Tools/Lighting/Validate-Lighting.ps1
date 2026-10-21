param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Config = 'Config/LightingBake.json',
    [string]$InputReplayPath = '',
    [int]$Frames = 600,
    [string]$Resolution = '1280x720'
)
$ErrorActionPreference = 'Stop'
$engineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
$settings = Get-Content -LiteralPath (Join-Path $projectRoot $Config) -Raw | ConvertFrom-Json
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot ($settings.output + '/manifest.json')) -Raw | ConvertFrom-Json
$evidence = Join-Path $projectRoot ('Saved/Diagnostics/Lighting/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $evidence
& (Join-Path $engineRoot 'hectl.bat') lighting check -Project $projectRoot -Source $Config
if ($LASTEXITCODE -ne 0) { throw 'Stale lighting data; rebake before comparison.' }
$reports = @()
foreach ($case in @(@{Name='source';Scene=$settings.scene}, @{Name='baked';Scene=$manifest.bakedScene})) {
    $capture = Join-Path $evidence ($case.Name + '.png')
    $log = Join-Path $evidence ($case.Name + '.log')
    $arguments = @('game','capture','-Project',$projectRoot,'-StartupScene',$case.Scene,
        '-Frames',[string]$Frames,'-Resolution',$Resolution,'-GraphicsApi','DX12','-StrictGraphicsApi',
        '-RenderValidation','-RenderStats','-RenderStatsPath',(Join-Path $evidence ($case.Name+'.csv')),
        '-CapturePath',$capture,'-TimeoutSeconds','600')
    if ($InputReplayPath) { $arguments += @('-InputReplayPath',$InputReplayPath) }
    & (Join-Path $engineRoot 'hectl.bat') @arguments *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 12; throw ($case.Name+' runtime failed') }
    # hectl reports its owned process stdout. Inspect it as well: a successful
    # process exit is insufficient if an asynchronous texture upload failed.
    $stdoutMatch = Select-String -LiteralPath $log -Pattern 'stdout: (.+?); stderr:' | Select-Object -Last 1
    if (-not $stdoutMatch) { throw 'Runtime stdout evidence missing' }
    $stdout = $stdoutMatch.Matches[0].Groups[1].Value
    $errors = @(Select-String -LiteralPath $stdout -Pattern '\[TextureUpload\].*failed|Failed to compile|\[error\]|D3D12 ERROR')
    if ($errors.Count) { throw ('Runtime resource errors; inspect '+$stdout) }
    $world = Get-Content -LiteralPath ($capture+'.world.json') -Raw | ConvertFrom-Json
    if ($case.Name -eq 'baked' -and $world.bakedIrradianceVolumeCount -ne $manifest.statistics.probes) {
        throw 'Baked irradiance components were not extracted into the rendered world'
    }
    $reports += @{case=$case.Name;scene=$case.Scene;capture=$capture;stdout=$stdout;
        irradianceVolumes=$world.bakedIrradianceVolumeCount;reflectionProbes=$world.reflectionProbeCount;status='PASS'}
}
$reports | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'validation.json') -Encoding UTF8
Write-Host ('runtime-manual PASS: '+$evidence)
