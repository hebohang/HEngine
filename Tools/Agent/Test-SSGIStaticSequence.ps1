[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$SettingsPath,
    [string]$Scene='Assets/RenderingBenchmarks/Validation/SSGILive/CurrentEditor.he',
    [string]$Resolution='1551x628', [ValidateRange(1,10000)][int]$WarmupFrames=240,
    [ValidateRange(2,120)][int]$Samples=12)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Use a new evidence directory.'}
New-Item -ItemType Directory -Path $output | Out-Null
$ctl=Join-Path $PSScriptRoot 'hectl.ps1'
try {
    & $ctl agent start -Configuration RelWithDebInfo -StartupScene $Scene -RenderPath Deferred -RenderSettingsPath $SettingsPath -Resolution $Resolution -RenderStats -RenderStatsPath (Join-Path $output 'frames.csv') -PerformanceWarmupFrames $WarmupFrames -TimeoutSeconds 180 *> (Join-Path $output 'start.log')
    if($LASTEXITCODE -ne 0){throw 'Agent start failed.'}
    $session=Get-Content (Join-Path $root 'out/agent/sessions/active.json') -Raw | ConvertFrom-Json
    for($remaining=$WarmupFrames; $remaining -gt 0; $remaining-=120) {
        & $ctl agent step -AgentStepFrames ([Math]::Min($remaining,120)) -TimeoutSeconds 120 *> (Join-Path $output "warmup-$remaining.json")
        if($LASTEXITCODE -ne 0){throw 'Warmup failed.'}
    }
    for($i=0;$i -lt $Samples;$i++) {
        & $ctl agent step -AgentStepFrames 1 *> (Join-Path $output "step-$i.json")
        if($LASTEXITCODE -ne 0){throw 'Step failed.'}
        $name='frame-{0:d2}.png' -f $i
        & $ctl agent capture -AgentCaptureName $name *> (Join-Path $output "capture-$i.json")
        if($LASTEXITCODE -ne 0){throw 'Capture failed.'}
        Copy-Item -LiteralPath (Join-Path $session.sessionDirectory "captures/$name") -Destination (Join-Path $output $name)
    }
    Write-Output "Captured $Samples static observations: $output"
} finally {
    & $ctl agent stop *> (Join-Path $output 'stop.log')
}
