[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(100,5000)][int]$IntervalMilliseconds=1000,
    [ValidateRange(1,3600)][int]$MaximumSeconds=1800,
    [switch]$SkipGpuQuery
)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$stopPath=Join-Path $OutputDirectory 'memory-sampler.stop'
$ownerPath=Join-Path $OutputDirectory 'memory-sampler-game.json'
$samplesPath=Join-Path $OutputDirectory 'memory-samples.jsonl'
if (Test-Path -LiteralPath $samplesPath) { throw 'Memory sample output already exists.' }
$nvidia=$null
if (!$SkipGpuQuery) { $nvidia=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue }
$writer=[IO.StreamWriter]::new($samplesPath,$false,[Text.UTF8Encoding]::new($false))
$clock=[Diagnostics.Stopwatch]::StartNew()
$sampleCount=0; $gpuSamples=0; $gameSamples=0
$gpuPeak=0.0; $privatePeak=0L; $workingSetPeak=0L
try {
    while (!(Test-Path -LiteralPath $stopPath) -and $clock.Elapsed.TotalSeconds -lt $MaximumSeconds) {
        $entry=[ordered]@{utc=[DateTime]::UtcNow.ToString('o'); gpuDeviceMemoryMiB=$null; game=$null}
        if ($nvidia) {
            $query=[Diagnostics.Process]::new()
            $query.StartInfo.FileName=$nvidia.Source
            $query.StartInfo.Arguments='-i 0 --query-gpu=memory.used --format=csv,noheader,nounits'
            $query.StartInfo.UseShellExecute=$false
            $query.StartInfo.CreateNoWindow=$true
            $query.StartInfo.RedirectStandardOutput=$true
            $query.StartInfo.RedirectStandardError=$true
            try {
                if ($query.Start()) {
                    if (!$query.WaitForExit(2000)) { $query.Kill(); $query.WaitForExit(); }
                    elseif ($query.ExitCode -eq 0) {
                        $value=0.0
                        if ([double]::TryParse($query.StandardOutput.ReadToEnd().Trim(),
                            [Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value)) {
                            $entry.gpuDeviceMemoryMiB=$value; $gpuSamples++
                            $gpuPeak=[Math]::Max($gpuPeak,$value)
                        }
                    }
                }
            } finally { $query.Dispose() }
        }
        if (Test-Path -LiteralPath $ownerPath) {
            $gameProcess=$null
            try {
                $owner=Get-Content -LiteralPath $ownerPath -Raw | ConvertFrom-Json
                $gameProcess=[Diagnostics.Process]::GetProcessById([int]$owner.processId)
                if ($gameProcess.StartTime.ToUniversalTime().Ticks -eq [long]$owner.startTicks -and
                    $gameProcess.MainModule.FileName -eq $owner.executablePath) {
                    $gameProcess.Refresh()
                    $entry.game=@{processId=$gameProcess.Id; privateBytes=$gameProcess.PrivateMemorySize64;
                        workingSetBytes=$gameProcess.WorkingSet64; peakWorkingSetBytes=$gameProcess.PeakWorkingSet64}
                    $gameSamples++; $privatePeak=[Math]::Max($privatePeak,$gameProcess.PrivateMemorySize64)
                    $workingSetPeak=[Math]::Max($workingSetPeak,$gameProcess.PeakWorkingSet64)
                }
            } catch { $entry.game=$null }
            finally { if($gameProcess){$gameProcess.Dispose()} }
        }
        $writer.WriteLine(($entry | ConvertTo-Json -Depth 4 -Compress)); $writer.Flush(); $sampleCount++
        Start-Sleep -Milliseconds $IntervalMilliseconds
    }
} finally { $writer.Dispose() }
$summary=@{samples=$sampleCount; gpuSamples=$gpuSamples; gameSamples=$gameSamples;
    deviceMemoryPeakMiB=$(if($gpuSamples -gt 0){$gpuPeak}else{$null});
    gpuSamplingEnabled=(!$SkipGpuQuery); gpuSamplesAvailable=($gpuSamples -gt 0);
    samplingMode=$(if($SkipGpuQuery){'process-only'}else{'process-and-optional-device-memory'});
    gpuSamplingStatus=$(if($SkipGpuQuery){'disabled-by-SkipGpuQuery'}elseif(!$nvidia){'nvidia-smi-unavailable'}elseif($gpuSamples -eq 0){'no-successful-query'}else{'sampled'});
    gamePrivatePeakBytes=$privatePeak; gameWorkingSetPeakBytes=$workingSetPeak;
    intervalMilliseconds=$IntervalMilliseconds; stoppedByOwner=(Test-Path -LiteralPath $stopPath);
    interpretation='GPU peak is null when unavailable, not zero usage. With SkipGpuQuery no nvidia-smi discovery or invocation occurs. When sampled, Device 0 memory includes other processes. Sampled peaks can miss short allocations; process working-set peak is an OS high-water mark. Samples include startup, captures and switching, not an isolated switch allocation peak.'}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'memory-summary.json'),
    ($summary | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
