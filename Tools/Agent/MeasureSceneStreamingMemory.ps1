[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Repository,
    [Parameter(Mandatory=$true)][string]$Prefix
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath($Repository)
$Prefix = [IO.Path]::GetFullPath($Prefix)
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out/agent')) +
    [IO.Path]::DirectorySeparatorChar
if (!$Prefix.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Prefix must be inside the selected repository out/agent directory.'
}
foreach ($suffix in @('.csv', '.process-memory.json')) {
    if (Test-Path -LiteralPath ($Prefix + $suffix)) {
        throw "Output already exists: $Prefix$suffix"
    }
}
$expected = [IO.Path]::GetFullPath((Join-Path $repo `
    'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'))
$oldTiming = [Environment]::GetEnvironmentVariable('HE_TEXTURE_UPLOAD_TIMINGS', 'Process')
$monitor = $null
$bStarted = $false
$bStopFailed = $false
$summary = $null
$startedUtc = [DateTime]::UtcNow
Push-Location $repo
try {
    [Environment]::SetEnvironmentVariable('HE_TEXTURE_UPLOAD_TIMINGS', '1', 'Process')
    & .\hectl.bat agent start `
        -StartupScene Assets/Scenes/MainMenu.he -GraphicsApi DX12 `
        -StrictGraphicsApi -RenderValidation -Resolution 1280x720 `
        -RenderStats -RenderStatsPath ($Prefix + '.csv') `
        -PerformanceWarmupFrames 0 -TimeoutSeconds 240
    if ($LASTEXITCODE -ne 0) { throw 'Agent start failed.' }
    $bStarted = $true
    $matches = @(Get-Process StarbloomSurvivor -ErrorAction SilentlyContinue |
        Where-Object {
            [StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $expected) -and
            $_.StartTime.ToUniversalTime() -ge $startedUtc
        })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one newly started owned Game process; found $($matches.Count)."
    }
    $game = $matches[0]
    try {
        $gamePid = $game.Id
        $gameStartTicks = $game.StartTime.ToUniversalTime().Ticks
    } finally { $game.Dispose() }
    $monitor = Start-Job -ArgumentList $gamePid, $gameStartTicks -ScriptBlock {
        param($observedPid, $expectedStartTicks)
        $peakWorkingSet = 0L
        $peakPrivateBytes = 0L
        $samples = 0
        while ($true) {
            $process = Get-Process -Id $observedPid -ErrorAction SilentlyContinue
            if (!$process) { break }
            try {
                if ($process.StartTime.ToUniversalTime().Ticks -ne [long]$expectedStartTicks) {
                    throw "Owned Game PID identity changed during sampling: actualTicks=$($process.StartTime.ToUniversalTime().Ticks) expectedTicks=$expectedStartTicks"
                }
                $process.Refresh()
                $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
                $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $process.PrivateMemorySize64)
                $samples++
            } finally { $process.Dispose() }
            Start-Sleep -Milliseconds 50
        }
        [pscustomobject]@{
            samples = $samples
            peakWorkingSetBytes = $peakWorkingSet
            sampledPeakPrivateBytes = $peakPrivateBytes
            pollIntervalMilliseconds = 50
        }
    }
    $json = @{path='Assets/Scenes/MainGame.he'} | ConvertTo-Json -Compress
    $null = & .\Tools\Agent\hectl.ps1 agent call `
        -Method runtime.scene.load -ParamsJson $json
    if ($LASTEXITCODE -ne 0) { throw 'Scene request failed.' }
    for ($group = 0; $group -lt 2; $group++) {
        $null = & .\hectl.bat agent step -AgentStepFrames 120
        if ($LASTEXITCODE -ne 0) { throw "Agent step group $group failed." }
    }
    $world = & .\hectl.bat agent world
    if ($LASTEXITCODE -ne 0 -or ($world | Out-String) -notmatch '"entityCount":437') {
        throw "Final world did not report 437 entities: $world"
    }
} finally {
    if ($bStarted) {
        $null = & .\hectl.bat agent stop
        $bStopFailed = $LASTEXITCODE -ne 0
    }
    if ($monitor) {
        $summary = Receive-Job -Job $monitor -Wait -AutoRemoveJob
    }
    [Environment]::SetEnvironmentVariable('HE_TEXTURE_UPLOAD_TIMINGS', $oldTiming, 'Process')
    Pop-Location
}
if ($bStopFailed) { throw 'Owned Agent stop failed.' }
if (!$summary -or $summary.samples -eq 0) {
    throw 'No owned Game memory samples were collected.'
}
$summary | Add-Member -NotePropertyName gameArtifactSha256 `
    -NotePropertyValue (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash
$summary | Add-Member -NotePropertyName statisticsPath `
    -NotePropertyValue ($Prefix + '.csv')
$summary | Add-Member -NotePropertyName interpretation `
    -NotePropertyValue 'Working set is the OS high-water mark; private bytes are sampled.'
[IO.File]::WriteAllText($Prefix + '.process-memory.json',
    ($summary | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$summary | ConvertTo-Json -Compress
