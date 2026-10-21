param([ValidateRange(1,20)][int]$Batches=5)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../..").Path
$busy=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('cl.exe','link.exe','ninja.exe','MSBuild.exe','cmake.exe')})
if ($busy.Count) { throw 'A compiler/build process is active. Run script profiling after builds finish.' }

$work=Join-Path $repo ('out/agent/scripting/pc-profile-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Process.ps1"
. "$repo/Tools/Agent/Storage.ps1"
$record=Start-HEStorageRun -Project "$repo/Project" -Workflow 'script-pc-profile' -Paths @($work) -Outer
$sessionRoot="$work/sessions"
$capture=$null; $exporter=$null; $state='failed'; $started=$false
function Session([string]$Action,[string]$Receipt,[string]$Method='session.status',[string]$Payload='{}') {
    $parameters=@{Action=$Action;SessionRoot=$sessionRoot;TimeoutSeconds=120}
    if($Action -eq 'Start') { $parameters.Configuration='RelWithDebInfo';$parameters.EnableTracy=$true;$parameters.StartupScene='Assets/WeChatSample/ScriptTouch.he' }
    if($Action -eq 'Call') { $parameters.Method=$Method;$parameters.ParamsJson=$Payload }
    $request="$work/$Receipt.request.json"
    @{scriptPath="$repo/Tools/Agent/Session.ps1";parameters=$parameters}|ConvertTo-Json -Depth 5|Set-Content $request -Encoding UTF8
    $process=Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSScriptRoot+'/InvokeProfileSession.ps1"'),'-RequestPath',('"'+$request+'"')) `
        -RedirectStandardOutput "$work/$Receipt.log" -RedirectStandardError "$work/$Receipt.stderr.log" -WindowStyle Hidden -PassThru
    try {
        $null=$process.Handle
        if(!$process.WaitForExit(180000)) { throw "Agent $Receipt timed out." }
        if($process.ExitCode -ne 0) { throw "Agent $Receipt failed: $work/$Receipt.stderr.log" }
    } finally { if(!$process.HasExited) { Stop-AgentOwnedProcess -Process $process -Description "profile session $Receipt" }; $process.Dispose() }
}
try {
    Session Start start
    $started=$true
    $owner=Get-Content "$sessionRoot/active.json" -Raw|ConvertFrom-Json
    if(!$owner.tracyEnabled) { throw 'The owned session is not a Tracy-enabled game.' }
    $managed=Get-Content (Join-Path (Split-Path $owner.artifact) 'Scripting/CSharp/build.json') -Raw|ConvertFrom-Json
    $artifactHash=(Get-FileHash -LiteralPath $owner.artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $owner|Select-Object pid,processStartUtcTicks,artifact,artifactSha256,tracyEnabled|ConvertTo-Json -Depth 5|Set-Content "$work/owner.json" -Encoding UTF8
    Session Call warmup runtime.step '{"frames":120}'
    $trace="$work/scripts.tracy"
    $capture=Start-Process -FilePath "$repo/Engine/Tools/Tracy/tracy-capture.exe" -ArgumentList @('-o',('"'+$trace+'"'),'-a','127.0.0.1','-p','8086') `
        -RedirectStandardOutput "$work/capture.stdout.log" -RedirectStandardError "$work/capture.stderr.log" -WindowStyle Hidden -PassThru
    $null=$capture.Handle
    $connected=$false; $deadline=[DateTime]::UtcNow.AddSeconds(30)
    while(!$connected -and [DateTime]::UtcNow -lt $deadline) {
        if($capture.HasExited) { throw 'Tracy capture exited before connecting.' }
        $connections=@(Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue)
        $client=@($connections|Where-Object {$_.OwningProcess -eq $capture.Id -and $_.RemotePort -eq 8086})
        $connected=$client.Count -eq 1 -and @($connections|Where-Object {$_.OwningProcess -eq $owner.pid -and $_.LocalPort -eq 8086 -and $_.RemotePort -eq $client[0].LocalPort}).Count -eq 1
        if(!$connected) { Start-Sleep -Milliseconds 200 }
    }
    if(!$connected) { throw 'Tracy did not connect to the owned game process.' }
    Start-Sleep -Seconds 1
    Session Call input input.set '{"held":["d"]}'
    for($batch=0;$batch -lt $Batches;$batch++) { Session Call "step-$batch" runtime.step '{"frames":120}' }
    Session Call clear input.clear
    Session Stop stop
    $started=$false
    if(!$capture.WaitForExit(60000)) { throw 'Tracy capture did not finish after game shutdown.' }
    $capture.WaitForExit()
    if($capture.ExitCode -ne 0 -or !(Test-Path $trace)) { throw 'Tracy failed to write a real trace.' }
    $exporter=Start-Process -FilePath "$repo/Engine/Tools/Tracy/tracy-csvexport.exe" -ArgumentList @('-u','-f','Script.',('"'+$trace+'"')) `
        -RedirectStandardOutput "$work/zones.csv" -RedirectStandardError "$work/export.stderr.log" -WindowStyle Hidden -PassThru
    $null=$exporter.Handle
    if(!$exporter.WaitForExit(60000)) { throw 'Tracy zone export timed out.' }
    $exporter.WaitForExit()
    if($exporter.ExitCode -ne 0) { throw 'Tracy zone export failed.' }
    $rows=@(Import-Csv "$work/zones.csv")
    $frames=@($rows|Where-Object name -eq 'Script.UpdateAndSnapshot'|Sort-Object {[long]$_.ns_since_start})
    if($frames.Count -ne $Batches*120) { throw "Expected $($Batches*120) real script frames, captured $($frames.Count)." }
    $zones=@()
    foreach($name in @('Script.UpdateAndSnapshot','Script.Synchronize','Script.Snapshot','Script.CSharp.Update','Script.CSharp.Dispatch','Script.EngineApi')) {
        $durations=[Collections.Generic.List[double]]::new(); $frameIndex=0
        foreach($event in @($rows|Where-Object name -eq $name|Sort-Object {[long]$_.ns_since_start})) {
            while($frameIndex -lt $frames.Count -and ([long]$frames[$frameIndex].ns_since_start+[long]$frames[$frameIndex].exec_time_ns) -le [long]$event.ns_since_start) { $frameIndex++ }
            if($frameIndex -ge $frames.Count) { break }
            $frame=$frames[$frameIndex]
            if($frame.thread -eq $event.thread -and [long]$event.ns_since_start -ge [long]$frame.ns_since_start -and
                ([long]$event.ns_since_start+[long]$event.exec_time_ns) -le ([long]$frame.ns_since_start+[long]$frame.exec_time_ns)) { $durations.Add([double]$event.exec_time_ns/1000) }
        }
        if(!$durations.Count) { throw "Missing real instrumented zone: $name" }
        $values=@($durations|Sort-Object)
        $zones += [ordered]@{name=$name;count=$values.Count;meanMicroseconds=($values|Measure-Object -Average).Average;
            p99Microseconds=$values[[Math]::Ceiling($values.Count*.99)-1];maxMicroseconds=$values[-1]}
    }
    [ordered]@{status='PASS';evidenceType='runtime-manual-tracy';frames=$frames.Count;zones=$zones;
        executableSha256=$artifactHash;gameplaySourceIdentity=$managed.gameplaySourceIdentity;gameScriptsSha256=$managed.gameScriptsSha256;
        limitations='Instrumented PC development game with synthetic held D input. Inclusive wall times and profiler overhead; nested zones must not be summed. No shipping interaction or GC-event attribution claim.'} |
        ConvertTo-Json -Depth 10|Set-Content "$work/result.json" -Encoding UTF8
    $state='success'
    Write-Output "runtime-manual script Tracy capture PASS: $work/result.json"
} finally {
    try {
        if($started -or (Test-Path "$sessionRoot/active.json")) { Session Stop cleanup-stop }
    } finally {
        if($capture -and !$capture.HasExited) { Stop-AgentOwnedProcess -Process $capture -Description 'script Tracy capture' }
        if($exporter -and !$exporter.HasExited) { Stop-AgentOwnedProcess -Process $exporter -Description 'script Tracy export' }
        Complete-HEStorageRun -Run $record -State $state
    }
}
