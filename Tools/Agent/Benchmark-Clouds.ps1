[CmdletBinding()]
param(
    [ValidateSet('default','sky','dense','contour')][string]$Scenario = 'default',
    [ValidateSet('Off','Low','Balanced','High')][string]$Quality = 'Balanced',
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [string]$OutputDirectory = '',
    [int]$Frames = 780
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$scenePath = Join-Path $repository 'Project/Assets/Scenes/MainGame.he'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repository ('out/agent/rendering-evidence/cloud-' + $Scenario + '-' + $Quality + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Cloud benchmark output already exists' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$run = Start-HEStorageRun -Project (Join-Path $repository 'Project') -Workflow 'cloud-benchmark' -Paths @($OutputDirectory) -Outer
$savedScene = [IO.File]::ReadAllBytes($scenePath)
$state = 'failed'
try {
    $text = [Text.Encoding]::UTF8.GetString($savedScene)
    $pitch = @{default='-20.0';sky='5.0';dense='5.0';contour='-12.0'}[$Scenario]
    $coverage = @{default='0.42';sky='0.42';dense='0.70';contour='0.62'}[$Scenario]
    $text = $text.Replace('"InitialPitchDegrees": -20.0', '"InitialPitchDegrees": ' + $pitch)
    $text = $text.Replace('"CloudCoverage": 0.42', '"CloudCoverage": ' + $coverage)
    [IO.File]::WriteAllText($scenePath, $text, [Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath $scenePath -Destination (Join-Path $OutputDirectory 'scene-input.he')
    $settingsPath = Join-Path $OutputDirectory 'settings.json'
    @{ 'Rendering.Clouds.Quality' = $Quality } | ConvertTo-Json | Set-Content -Encoding utf8 $settingsPath
    @{ scenario=$Scenario;quality=$Quality;scene='Assets/Scenes/MainGame.he';
        sceneSha256=(Get-FileHash -LiteralPath $scenePath).Hash;runSeed=7319;cloudSeed=7301;
        fixedDeltaSeconds=0.0083333333;input='No held keys; real gameplay simulation and enemy waves continue at fixed 120 Hz';
        pitch=[double]$pitch;coverage=[double]$coverage;warmupFrames=120;frames=$Frames;
        cpuWorkTiming='cpuCloudRecordMs: cloud prepare/import/graph construction/NVRHI recording scopes; excludes frame slot wait, queue submit, present and common RenderGraph overhead';
        texturePayloadBytes=($(if ($Quality -eq 'Off') {0} elseif ($Quality -eq 'Low') {480*270*48+64*64*64} else {960*540*48+64*64*64}))
    } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $OutputDirectory 'input.json')
    # Keep VS environment setup local to the child; repeated direct Build.ps1
    # invocations must not progressively alter this evidence owner's PATH.
    $shellExe = (Get-Process -Id $PID).Path
    & $shellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hectl.ps1') game capture -StartupScene Assets/Scenes/MainGame.he `
        -Resolution 1920x1080 -Frames $Frames -FixedDeltaSeconds 0.0083333333 -PerformanceWarmupFrames 120 -RenderStats `
        -RenderStatsPath (Join-Path $OutputDirectory 'frames.csv') -RenderSettingsPath $settingsPath `
        -CapturePath (Join-Path $OutputDirectory 'frame.png') -GraphicsApi $GraphicsApi -StrictGraphicsApi -TimeoutSeconds 180
    if ($LASTEXITCODE -ne 0) { throw 'Cloud benchmark run failed' }
    $state = 'success'
}
finally {
    [IO.File]::WriteAllBytes($scenePath, $savedScene)
    # Scene restoration is part of completion; retain evidence until it finishes.
    Complete-HEStorageRun -Run $run -State $state
}
