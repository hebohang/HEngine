param(
    [string]$Project = 'C:/WorkSpace/HEngineProjects/MechanicGirlLab',
    [string]$Label = 'baseline',
    [string]$SettingsPath = '',
    [switch]$Fullscreen,
    [switch]$RespectPresentationSettings
)
$ErrorActionPreference = 'Stop'
$engine = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$current = & (Join-Path $engine 'hectl.bat') project current -Project $Project -Json | ConvertFrom-Json
$output = Join-Path $Project ('Saved/Diagnostics/TemporalStability/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Label)
New-Item -ItemType Directory -Path $output -Force | Out-Null
Get-FileHash (Join-Path $engine 'Engine/Content/Shaders/Modern/SceneTaa.hlsl'), (Join-Path $engine 'Engine/Content/Shaders/Modern/SceneTaaReconstruct.hlsl') | ConvertTo-Json | Set-Content (Join-Path $output 'shader-sources.json') -Encoding utf8
$session = Join-Path $PSScriptRoot 'Session.ps1'
$common = @{
    ProjectRoot = $Project
    ArtifactPath = Join-Path $current.gameBuild ('bin/' + $current.project.ExecutableName + '.exe')
    SessionRoot = $current.agentSessions
    OwnerTag = [guid]::NewGuid().ToString('N')
    Configuration = 'RelWithDebInfo'
    TimeoutSeconds = 300
    RespectPresentationSettings = $RespectPresentationSettings
}
function Call([string]$method, [hashtable]$parameters = @{}) {
    $lines = @(& $session @common -Action Call -Method $method -ParamsJson ($parameters | ConvertTo-Json -Depth 12 -Compress))
    $reply = $lines | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1 | ConvertFrom-Json
    if (-not $reply.ok) { throw "$method failed: $($reply.error | ConvertTo-Json -Compress)" }
    return $reply.result
}
$started = $false
$display = if ($Fullscreen) { @{UseProjectDisplaySettings=$true} } else { @{Resolution='3840x2160'} }
try {
    & $session @common @display -Action Start -StartupScene 'Assets/Scenes/SickaDynasty.he' -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -RenderSettingsPath $SettingsPath -RenderStats -RenderStatsPath (Join-Path $output 'frames.csv') *> (Join-Path $output 'start.log')
    $started = $true
    $null = Call 'runtime.step' @{frames=120}
    $null = Call 'runtime.step' @{frames=120}
    Call 'render.settings.get' | ConvertTo-Json -Depth 32 | Set-Content (Join-Path $output 'settings.json') -Encoding utf8
    $sequence = Call 'capture.sequence' @{prefix='static';frames=16;cameraCut=$true;settleFrames=64}
    $sequence | ConvertTo-Json -Depth 32 | Set-Content (Join-Path $output 'static.json') -Encoding utf8
    $null = Call 'input.set' @{held=@('w')}
    $sequence = Call 'capture.sequence' @{prefix='moving';frames=16}
    $sequence | ConvertTo-Json -Depth 32 | Set-Content (Join-Path $output 'moving.json') -Encoding utf8
    $null = Call 'input.clear'
    $null = Call 'input.pointer' @{x=1900;y=1080;rightHeld=$false}
    $null = Call 'runtime.step' @{frames=1}
    # Advance the pointer on the same simulation steps as the atomic sequence.
    # Separate capture.frame calls allow extra render submissions between images.
    $null = Call 'input.pointer' @{x=1920;y=1080;rightHeld=$true;steps=16;stepX=20;stepY=0}
    $sequence = Call 'capture.sequence' @{prefix='turn';frames=16}
    $sequence | ConvertTo-Json -Depth 32 | Set-Content (Join-Path $output 'turn.json') -Encoding utf8
    $null = Call 'input.clear'
    Get-FileHash $common.ArtifactPath | ConvertTo-Json | Set-Content (Join-Path $output 'binary.json')
    Write-Output "runtime-manual sequence evidence: $output"
} finally {
    if ($started) { & $session @common -Action Stop *> (Join-Path $output 'stop.log') }
}
