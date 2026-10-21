param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("baseline", "gpu20k", "gpu50k", "cpu20k-proxy", "cpu50k-proxy")]
    [string]$Scenario,

    [ValidateSet("Low", "Medium", "High")]
    [string]$Quality = "High",

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "RelWithDebInfo",

    [ValidateSet("DX12", "Vulkan")]
    [string]$GraphicsApi = "DX12",

    [ValidatePattern("^[1-9][0-9]{0,3}[xX][1-9][0-9]{0,3}$")]
    [string]$Resolution = "1280x720",

    [ValidateRange(1.0, 60.0)]
    [double]$WarmupSeconds = 5.0,

    [ValidateRange(5.0, 120.0)]
    [double]$SampleSeconds = 10.0,

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 240,

    [switch]$EnableTracy,

    [switch]$Capture,

    [string]$CapturePath = "",

    [ValidateRange(0, 3600)]
    [int]$CaptureFrames = 0,

    [switch]$IUnderstandThisTemporarilyChangesGameConfig
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$gameConfigPath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot "Project\Config\Game.json"))
$expectedConfigPath = Join-Path $repositoryRoot "Project\Config\Game.json"
$stressScenePath = Join-Path $repositoryRoot "Project\Assets\Scenes\VFXStressTest.he"
$hectlPath = Join-Path $repositoryRoot "hectl.bat"
$stateDirectory = Join-Path $repositoryRoot "out\agent\vfx-stress"
$backupPath = Join-Path $stateDirectory "Game.json.backup"
$markerPath = Join-Path $stateDirectory "active.json"

# Deprecated compatibility switch: startup selection no longer mutates Game.json.
. (Join-Path $repositoryRoot "Tools/Agent/Storage.ps1")

if (-not [System.String]::Equals(
        $gameConfigPath,
        [System.IO.Path]::GetFullPath($expectedConfigPath),
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved Game.json escaped the repository."
}
if (-not (Test-Path -LiteralPath $gameConfigPath -PathType Leaf)) {
    throw "Missing Game config: $gameConfigPath"
}
if (-not (Test-Path -LiteralPath $stressScenePath -PathType Leaf)) {
    throw "Missing stress scene: $stressScenePath"
}
if (-not (Test-Path -LiteralPath $hectlPath -PathType Leaf)) {
    throw "Missing hectl: $hectlPath"
}

New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null

# Recover an interrupted prior run before observing the current config. The marker
# stores the temporary config hash; restoration is refused if another writer has
# changed the file since this runner installed its temporary content.
if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
        throw "VFX stress recovery marker exists without Game.json backup: $markerPath"
    }
    $marker = Get-Content -Raw -Encoding UTF8 -LiteralPath $markerPath | ConvertFrom-Json
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $currentHash = [BitConverter]::ToString($hasher.ComputeHash(
            [System.IO.File]::ReadAllBytes($gameConfigPath))).Replace('-', '')
    }
    finally { $hasher.Dispose() }
    if (-not [System.String]::Equals(
            $currentHash,
            [string]$marker.temporarySha256,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Game.json changed after an interrupted VFX stress run; refusing automatic recovery."
    }
    [System.IO.File]::WriteAllBytes($gameConfigPath, [System.IO.File]::ReadAllBytes($backupPath))
    Remove-Item -LiteralPath $markerPath -Force -ErrorAction Stop
    Remove-Item -LiteralPath $backupPath -Force -ErrorAction Stop
    Write-Host "Recovered Game.json from an interrupted VFX stress run."
}

$environmentNames = @(
    "HE_VFX_STRESS_MODE",
    "HE_VFX_STRESS_EFFECT",
    "HE_VFX_STRESS_SCENARIO",
    "HE_VFX_STRESS_QUALITY",
    "HE_VFX_STRESS_WARMUP_SECONDS",
    "HE_VFX_STRESS_SAMPLE_SECONDS",
    "HE_VFX_STRESS_SEED",
    "HE_VFX_STATS_LOG",
    "HE_VFX_GPU_PARTICLES"
)
$originalEnvironment = @{}
foreach ($name in $environmentNames) {
    $originalEnvironment[$name] = [System.Environment]::GetEnvironmentVariable($name, "Process")
}

$runDirectory = Join-Path $stateDirectory ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$storageRun = Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'vfx-stress' -Paths @($runDirectory) -Outer
$storageState = 'failed'
$exitCode = 1
try {
    $env:HE_VFX_STRESS_MODE = "1"
    $env:HE_VFX_STRESS_EFFECT = $null
    $env:HE_VFX_STRESS_SCENARIO = $Scenario
    $env:HE_VFX_STRESS_QUALITY = $Quality
    $env:HE_VFX_STRESS_WARMUP_SECONDS = $WarmupSeconds.ToString(
        "R", [System.Globalization.CultureInfo]::InvariantCulture)
    $env:HE_VFX_STRESS_SAMPLE_SECONDS = $SampleSeconds.ToString(
        "R", [System.Globalization.CultureInfo]::InvariantCulture)
    $env:HE_VFX_STRESS_SEED = "1806"
    $env:HE_VFX_STATS_LOG = "1"
    $env:HE_VFX_GPU_PARTICLES = if ($Scenario.StartsWith("gpu", [System.StringComparison]::Ordinal)) {
        "1"
    } else {
        "0"
    }

    $totalSeconds = $WarmupSeconds + $SampleSeconds + 1.0
    $frames = if ($Capture -and $CaptureFrames -gt 0) {
        [long]$CaptureFrames
    } else {
        [long][Math]::Ceiling($totalSeconds * 60.0)
    }
    $runAction = if ($Capture) { "capture" } else { "check" }
    $arguments = @(
        "game", $runAction,
        "-Configuration", $Configuration,
        "-Frames", $frames,
        "-FixedDeltaSeconds", "0.016666666666666666",
        "-Resolution", $Resolution,
        "-TimeoutSeconds", $TimeoutSeconds,
        "-GraphicsApi", $GraphicsApi,
        "-StrictGraphicsApi",
        "-StartupScene", "Assets/Scenes/VFXStressTest.he"
    )
    if ($EnableTracy) {
        $arguments += "-EnableTracy"
    }
    if ($Capture -and -not [string]::IsNullOrWhiteSpace($CapturePath)) {
        $arguments += @("-CapturePath", $CapturePath)
    }

    Write-Host "VFX stress scenario=$Scenario quality=$Quality resolution=$Resolution frames=$frames seed=1806"
    [ordered]@{scenario=$Scenario; quality=$Quality; configuration=$Configuration; backend=$GraphicsApi; arguments=$arguments} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'invocation.json') -Encoding UTF8
    & $hectlPath @arguments 2>&1 | Tee-Object -FilePath (Join-Path $runDirectory 'run.log')
    $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
    if ($exitCode -eq 0) { $storageState = 'success' }
    [ordered]@{exitCode=$exitCode; state=$storageState} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $runDirectory 'result.json') -Encoding UTF8
}
finally {
    foreach ($name in $environmentNames) {
        [System.Environment]::SetEnvironmentVariable(
            $name, $originalEnvironment[$name], "Process")
    }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}

exit $exitCode
