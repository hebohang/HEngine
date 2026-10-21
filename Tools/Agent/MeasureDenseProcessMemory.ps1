[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Prefix,
    [string]$Repository='')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath($(if($Repository){$Repository}else{Join-Path $PSScriptRoot '../..'}))
$Prefix=[IO.Path]::GetFullPath($Prefix)
$outputRoot=[IO.Path]::GetFullPath((Join-Path $repo 'out/agent'))+[IO.Path]::DirectorySeparatorChar
if (!$Prefix.StartsWith($outputRoot,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Prefix must be inside the selected repository out/agent directory.'
}
foreach($suffix in @('.csv','.process-memory.json','.hectl.stdout.log')) {
    if(Test-Path -LiteralPath ($Prefix+$suffix)) { throw "Output already exists: $Prefix$suffix" }
}
$expected=[IO.Path]::GetFullPath((Join-Path $repo 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'))
$stdout="$Prefix.hectl.stdout.log"
$stderr="$Prefix.hectl.stderr.log"
$stats="$Prefix.csv"
$wrapper=$null
$gameId=0
$startedUtc=[DateTime]::UtcNow
$peakWorkingSet=0L
$peakPrivateBytes=0L
$count=0
$samples=[Collections.Generic.List[object]]::new()
$oldScenario=[Environment]::GetEnvironmentVariable('HE_SURVIVOR_CAPTURE_SCENARIO','Process')
try {
    [Environment]::SetEnvironmentVariable('HE_SURVIVOR_CAPTURE_SCENARIO','region_4_final','Process')
    $args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',
        (Join-Path $repo 'Tools/Agent/hectl.ps1'),'game','check',
        '-Frames','500','-PerformanceWarmupFrames','120',
        '-StartupScene','Assets/Scenes/MainGame.he',
        '-GraphicsApi','DX12','-StrictGraphicsApi','-RenderValidation',
        '-Resolution','1280x720','-FixedDeltaSeconds','0.0166667',
        '-RenderStats','-RenderStatsPath',$stats,'-TimeoutSeconds','240')
    $start=@{FilePath='powershell.exe';ArgumentList=$args;WorkingDirectory=$repo;
        WindowStyle='Hidden';PassThru=$true;RedirectStandardOutput=$stdout;
        RedirectStandardError=$stderr}
    $wrapper=Start-Process @start
    while (!$wrapper.HasExited) {
        if (!$gameId -and (Test-Path -LiteralPath $stdout)) {
            $match=Select-String -LiteralPath $stdout -Pattern 'run HGame: STARTED pid=(\d+)' |
                Select-Object -Last 1
            if ($match) { $gameId=[int]$match.Matches[0].Groups[1].Value }
        }
        if ($gameId) {
            $game=Get-Process -Id $gameId -ErrorAction SilentlyContinue
            if ($game) {
                try {
                    $actualPath=$game.Path
                    $actualStart=$game.StartTime.ToUniversalTime()
                    if ([string]::IsNullOrWhiteSpace($actualPath)) { continue }
                    if (![StringComparer]::OrdinalIgnoreCase.Equals($actualPath,$expected) -or
                        $actualStart -lt $startedUtc) {
                        throw "Observed Game PID does not belong to this run: actualPath=$actualPath expected=$expected started=$actualStart notBefore=$startedUtc"
                    }
                    $game.Refresh()
                    $peakWorkingSet=[Math]::Max($peakWorkingSet,$game.PeakWorkingSet64)
                    $peakPrivateBytes=[Math]::Max($peakPrivateBytes,$game.PrivateMemorySize64)
                    $samples.Add(@{utc=[DateTime]::UtcNow.ToString('o');workingSetBytes=$game.WorkingSet64;
                        peakWorkingSetBytes=$game.PeakWorkingSet64;privateBytes=$game.PrivateMemorySize64})
                    $count++
                } finally { $game.Dispose() }
            }
        }
        Start-Sleep -Milliseconds 50
    }
    $wrapper.WaitForExit()
    $wrapper.Refresh()
    if ($null -ne $wrapper.ExitCode -and $wrapper.ExitCode -ne 0) {
        throw "hectl check failed: $($wrapper.ExitCode)"
    }
    $passed=Select-String -LiteralPath $stdout -Pattern '^run HGame: PASS exit=0$' -Quiet
    if (!$passed) { throw 'hectl output did not report a passing Game run.' }
    if (!$gameId -or !$count) { throw 'No owned Game process memory was sampled.' }
    $summary=@{gamePid=$gameId;samples=$count;peakWorkingSetBytes=$peakWorkingSet;
        sampledPeakPrivateBytes=$peakPrivateBytes;pollIntervalMilliseconds=50;
        gameArtifactSha256=(Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash;
        statisticsPath=$stats;interpretation='Working set is OS high-water mark; private bytes are sampled.'}
    [IO.File]::WriteAllText("$Prefix.process-memory.json",
        ($summary|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$Prefix.process-memory-samples.json",
        ($samples|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    $summary|ConvertTo-Json -Compress
} finally {
    [Environment]::SetEnvironmentVariable('HE_SURVIVOR_CAPTURE_SCENARIO',$oldScenario,'Process')
    if ($wrapper) { if (!$wrapper.HasExited) { $wrapper.WaitForExit() }; $wrapper.Dispose() }
}
