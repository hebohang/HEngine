param(
    [string]$Cli = $env:HE_WECHAT_CLI,
    [string]$EvidenceDirectory = '',
    [string]$ProjectDirectory = '',
    [switch]$KeepOpen
)
$ErrorActionPreference = 'Stop'
if (!$Cli) { $Cli = (Get-Command wechatide.cmd -ErrorAction SilentlyContinue).Source }
if (!$Cli -or !(Test-Path -LiteralPath $Cli)) { throw 'Set HE_WECHAT_CLI or pass -Cli with the installed wechatide.cmd path.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$ProjectDirectory) { $ProjectDirectory = Join-Path $repo 'out/agent/scripting/game-wechat' }
$project = (Resolve-Path -LiteralPath $ProjectDirectory).Path
if (!$EvidenceDirectory) { $EvidenceDirectory = Join-Path $repo ('out/agent/scripting/wechat-game-evidence-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force $EvidenceDirectory | Out-Null
. (Join-Path $repo 'Tools/Agent/Storage.ps1')
$storageRun = Start-HEStorageRun -Project (Join-Path $repo 'Project') -Workflow 'scripting-wechat-validation' -Paths @($EvidenceDirectory) -Outer
$checks = [Collections.Generic.List[string]]::new()
function Invoke-GameTool([string]$Name, [string[]]$Arguments, [string]$Receipt) {
    $previous = $ErrorActionPreference
    try { $ErrorActionPreference='Continue'; $lines = & $Cli -c Codex $Name --project $project @Arguments 2>&1; $code=$LASTEXITCODE }
    finally { $ErrorActionPreference=$previous }
    $text = $lines -join "`n"
    $text | Set-Content -LiteralPath (Join-Path $EvidenceDirectory ($Receipt+'.log'))
    $start = $text.IndexOf('{')
    if ($code -ne 0 -or $start -lt 0) { throw "WeChat command failed: $Receipt" }
    $result = $text.Substring($start) | ConvertFrom-Json
    if (!$result.ok -or $result.result.success -eq $false) { throw "WeChat operation failed: $Receipt; $($result.message)" }
    return $result.result
}
function Evaluate([string]$Code, [string]$Receipt) {
    # DevTools automation runs in its outer context; the real game owns this window.
    $fn = 'async function(){const s=globalThis.__gameContextWindow.HEWeChatSession;'+$Code+'}'
    return (Invoke-GameTool 'automation_evaluate' @('--fn-source',$fn) $Receipt).result
}
function Assert-Game([bool]$Condition,[string]$Name) {
    if (!$Condition) { throw "Game assertion failed: $Name" }
    $checks.Add($Name)
}
$result = [ordered]@{status='FAIL'; evidenceType='runtime-manual-devtools-automation'; checks=$checks; visualReview='PENDING'}
try {
    # Opening a lite-mode window need not compile a freshly exported package.
    # Request compilation explicitly; its receipt alone is not a readiness gate.
    $null = Invoke-GameTool 'open_project_window' @('--window-mode','liteMode') 'open'
    $null = Invoke-GameTool 'simulator_refresh' @() 'compile-refresh'
    # Opening a project window completes before its game context/Mono runtime is
    # ready. Wait for real rendered frames, not merely a successful IDE open.
    $readyFunction = 'function(){const s=globalThis.__gameContextWindow?.HEWeChatSession;return !!(s?.scripts&&s.getMetrics().frames>0);}'
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $ready = $false
    $attempt = 0
    do {
        try { $ready = (Invoke-GameTool 'automation_evaluate' @('--fn-source',$readyFunction) ('ready-' + $attempt)).result -eq $true }
        catch {
            if ($_.Exception.Message -notmatch 'timeout waiting for automator response') { throw }
            # A refresh before the lite-mode simulator connects can be lost.
            # Retry once; only observed engine frames can satisfy readiness.
            if ($attempt -eq 0) { $null = Invoke-GameTool 'simulator_refresh' @() 'refresh-after-connect' }
        }
        $attempt++
        if (!$ready) { Start-Sleep -Milliseconds 500 }
    } while (!$ready -and [DateTime]::UtcNow -lt $deadline)
    if (!$ready) { throw 'Timed out waiting for a real C# engine frame; inspect ready-* receipts.' }
    # A prior deliberate fault is a persisted editable field. Start this
    # integration fixture from its authored scene, retaining prior state in the
    # evidence instead of treating an already faulted instance as input-ready.
    $fixture = Evaluate 'const previous={state:s.scripts.snapshot(),metrics:s.getMetrics()};await s.reload(false);return previous;' 'fixture-reset'
    $initial = Evaluate 'return {state:s.scripts.snapshot(),metrics:s.getMetrics(),memory:s.scripts.memory(),identity:s.scripts.identity()};' 'initial'
    Assert-Game ($initial.metrics.frames -gt 0 -and $initial.metrics.errors.Count -eq 0) 'real-engine-frames'
    $result.identity=$initial.identity; $result.memory=$initial.memory
    $result.startup=$fixture.metrics.firstRenderedFrameMs
    $result.reentryStartup=$initial.metrics.firstRenderedFrameMs
    $null = Invoke-GameTool 'automation_game_action' @('--action','swipe','--start-x','280','--start-y','440','--end-x','340','--end-y','350','--duration','400') 'touch'
    $after = Evaluate 'return s.scripts.snapshot();' 'after-touch'
    $firstFields = $initial.state.script.Fields | ConvertFrom-Json
    $afterFields = $after.script.Fields | ConvertFrom-Json
    Assert-Game ($afterFields.fields.score.value -eq $firstFields.fields.score.value+3) 'real-touch-csharp-score'
    Assert-Game (($initial.state.transform.Translation | ConvertTo-Json -Compress) -ne ($after.transform.Translation | ConvertTo-Json -Compress)) 'real-touch-movement'
    $null = Invoke-GameTool 'simulator_screenshot' @('--path',(Join-Path $EvidenceDirectory 'touched.jpg')) 'screenshot'
    if (!(Test-Path -LiteralPath (Join-Path $EvidenceDirectory 'touched.jpg') -PathType Leaf)) { throw 'DevTools did not save the requested game screenshot.' }
    $disabled = Evaluate 's.scripts.enabled(false);return s.scripts.snapshot();' 'disabled'
    $null = Invoke-GameTool 'automation_game_action' @('--action','swipe','--start-x','120','--start-y','400','--end-x','100','--end-y','300','--duration','250') 'disabled-touch'
    $still = Evaluate 'return s.scripts.snapshot();' 'disabled-after'
    Assert-Game (($disabled.transform | ConvertTo-Json -Compress) -eq ($still.transform | ConvertTo-Json -Compress)) 'disabled-touch-no-movement'
    $reloaded = Evaluate 'await s.reload(true);return s.scripts.snapshot();' 'reloaded'
    Assert-Game (!$reloaded.script.Enabled) 'disabled-state-restored'
    Assert-Game (($disabled.sprite | ConvertTo-Json -Compress) -eq ($reloaded.sprite | ConvertTo-Json -Compress)) 'sprite-restored'
    Assert-Game (($disabled.transform | ConvertTo-Json -Compress) -eq ($reloaded.transform | ConvertTo-Json -Compress)) 'transform-restored'
    $enabled = Evaluate 's.scripts.enabled(true);return s.scripts.snapshot();' 'enabled'
    $fields = $enabled.script.Fields | ConvertFrom-Json
    Assert-Game ($fields.fields.enableCount.value -eq $afterFields.fields.enableCount.value+1) 'reenabled-once'
    $gc = Evaluate 'const values=[];for(let i=0;i<20;i++)values.push(s.scripts.collect());return {values,metrics:s.scripts.metrics(),memory:s.scripts.memory()};' 'gc'
    $result.gc=$gc
    $oldFaults = Invoke-GameTool 'get_simulator_console' @('--command','grep -n .') 'fault-baseline'
    $beforeFault = Evaluate 's.scripts.fault();return s.getMetrics().frames;' 'fault'
    $null = Invoke-GameTool 'automation_game_action' @('--action','tap','--x','300','--y','420','--wait','1') 'fault-touch'
    $afterFault = Evaluate 'return {frames:s.getMetrics().frames,errors:s.getMetrics().errors};' 'fault-after'
    Assert-Game ($afterFault.frames -gt $beforeFault -and $afterFault.errors.Count -eq 0) 'engine-continues-after-script-exception'
    $faults = Invoke-GameTool 'get_simulator_console' @('--command','grep -n .') 'fault-stack'
    # Count diagnostic records, not repeated message text in an exception chain.
    Assert-Game (([regex]::Matches($faults,'\[Script\] instance=\d+, kind=\d+:').Count - [regex]::Matches($oldFaults,'\[Script\] instance=\d+, kind=\d+:').Count) -eq 1) 'one-script-fault-diagnostic'
    Assert-Game ($faults -match 'GameScripts.ScriptTouchSample.Update') 'managed-gameplay-stack'
    $null = Evaluate 'await s.reload(false);s.scripts.destroy();return s.getMetrics();' 'destroy'
    $destroyedFrame = Evaluate 'return s.getMetrics().frames;' 'destroyed-frame'
    $null = Invoke-GameTool 'automation_game_action' @('--action','tap','--x','300','--y','420','--wait','1') 'destroyed-touch'
    $stillAlive = Evaluate 'return {ready:s.isReady(),frames:s.getMetrics().frames};' 'destroyed-touch-after'
    Assert-Game ($stillAlive.ready -and $stillAlive.frames -gt $destroyedFrame) 'touch-after-destroy-keeps-engine-alive'
    $reentered = Evaluate 'await s.reload(false);return s.scripts.snapshot();' 'reentered'
    Assert-Game ($null -ne $reentered.script) 'destroy-and-scene-reentry'
    $null = Invoke-GameTool 'get_simulator_console' @('--command','grep -n .') 'console'
    $result.status='PASS'
} catch { $result.error=$_.Exception.Message; throw }
finally {
    try {
        $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'result.json')
        if (!$KeepOpen) { $null = Invoke-GameTool 'close_project_window' @() 'close' }
    } finally {
        $state = if ($KeepOpen) { 'unknown' } elseif ($result.status -eq 'PASS') { 'success' } else { 'failed' }
        Complete-HEStorageRun -Run $storageRun -State $state
    }
}
Write-Host "runtime-manual DevTools scripting checks PASS: $EvidenceDirectory"
