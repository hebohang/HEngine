param([Parameter(Mandatory=$true)][string]$Cli)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../..").Path
$project=Join-Path $repo 'out/agent/scripting/game-wechat'
$work=Join-Path $repo ('out/agent/scripting/wechat-performance-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
if (@(Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('cl.exe','link.exe','ninja.exe','MSBuild.exe','cmake.exe') -or ($_.Name -match '^python.*[.]exe$' -and $_.CommandLine -match 'storage[.]py\s+(auto|stale)')}).Count) { throw 'Wait for active build or storage cleanup processes before measuring.' }
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record=Start-HEStorageRun -Project "$repo/Project" -Workflow 'wechat-performance' -Paths @($work) -Outer
$state='failed'
function Invoke-MeasureTool([string]$Name,[string[]]$Arguments,[string]$Receipt) {
    $previous=$ErrorActionPreference
    try { $ErrorActionPreference='Continue'; $lines=& $Cli -c Codex $Name --project $project @Arguments 2>&1; $code=$LASTEXITCODE }
    finally { $ErrorActionPreference=$previous }
    $text=$lines -join "`n"; Set-Content "$work/$Receipt.log" $text
    $start=$text.IndexOf('{')
    if ($code -ne 0 -or $start -lt 0) { throw "DevTools command failed: $Receipt" }
    $value=$text.Substring($start)|ConvertFrom-Json
    if (!$value.ok -or $value.result.success -eq $false) { throw "DevTools operation failed: $Receipt; $($value.message)" }
    return $value.result
}
function Evaluate([string]$Function,[string]$Receipt) { return (Invoke-MeasureTool automation_evaluate @('--fn-source',$Function) $Receipt).result }
try {
    $null=Invoke-MeasureTool open_project_window @('--window-mode','liteMode') 'open'
    $null=Invoke-MeasureTool simulator_refresh @() 'refresh'
    $ready=$false
    $deadline=[DateTime]::UtcNow.AddSeconds(90)
    for ($attempt=0;[DateTime]::UtcNow -lt $deadline -and !$ready;$attempt++) {
        try { $ready=(Evaluate 'function(){const s=globalThis.__gameContextWindow?.HEWeChatSession;return !!(s?.scripts&&s.getMetrics().frames>30);}' "ready-$attempt") -eq $true }
        catch {
            if ($_.Exception.Message -notmatch 'timeout waiting for automator response') { throw }
            # A refresh sent before the lite-mode simulator connects can be lost.
            # Retry once, but still require observed engine frames afterwards.
            if ($attempt -eq 0) { $null=Invoke-MeasureTool simulator_refresh @() 'refresh-after-connect' }
        }
        if (!$ready) { Start-Sleep -Milliseconds 500 }
    }
    if (!$ready) { throw 'No real C# engine frames.' }
    $null=Evaluate 'async function(){const s=globalThis.__gameContextWindow.HEWeChatSession;const before=s.scripts.snapshot();await s.reload(false);return before;}' 'reset'
    # The Windows .cmd CLI transports one function argument; embedded newlines
    # are not preserved reliably through its command-line bridge.
    $probe=(Get-Content "$PSScriptRoot/WeChatPerformanceProbe.js" -Raw) -replace '\r?\n',' '
    $null=Evaluate $probe 'begin'
    $done=$false
    for ($attempt=0;$attempt -lt 45 -and !$done;$attempt++) {
        Start-Sleep -Seconds 1
        $done=(Evaluate 'function(){return globalThis.__gameContextWindow.__hePerformanceProbe.done;}' "poll-$attempt") -eq $true
    }
    if (!$done) { throw 'Real-frame performance capture timed out.' }
    $result=Evaluate 'function(){return globalThis.__gameContextWindow.__hePerformanceProbe;}' 'capture'
    $result | Add-Member measurement 'Windows WeChat DevTools, real C++ engine and Mono. Observer RAF intervals use Date.now (1ms granularity) and include host scheduling/collection overhead. Host cpuFrameMs covers at most its first 300 frames after 30 warmup frames, not each phase. Bridge totals include actual boundary calls; disabled phase is not a C++ gameplay baseline. Linear memory sizes are not process RSS, mobile memory or independent heap totals.'
    $expected=Get-Content "$project/game-manifest.json" -Raw|ConvertFrom-Json
    if ($result.identity.buildId -ne $expected.buildId) { $result.status='FAIL'; $result | Add-Member error 'Measured a stale WeChat package.' -Force }
    $result | ConvertTo-Json -Depth 30 | Set-Content "$work/result.json" -Encoding UTF8
    if ($result.status -ne 'PASS') { throw "Performance probe failed: $($result.error)" }
    $state='success'
    Write-Output "runtime-manual WeChat DevTools measurement PASS: $work/result.json"
} finally {
    try { $null=Invoke-MeasureTool close_project_window @() 'close' }
    finally { Complete-HEStorageRun -Run $record -State $state }
}
