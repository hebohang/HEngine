param([string]$EvidenceDirectory = '', [ValidateRange(1,65535)][int]$Port = 8771, [string]$PackageDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
if (!$PackageDirectory) { $PackageDirectory = "$repo/out/agent/scripting/game-web" }
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path
if (!$EvidenceDirectory) { $EvidenceDirectory = "$repo/out/agent/scripting/web-evidence-$(Get-Date -Format yyyyMMdd-HHmmss-fff)" }
$work = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Process.ps1"
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'scripting-web-validation' -Paths @($work) -Outer
$node = $env:HE_SCRIPT_NODE
if (!$node) { $node = "$env:USERPROFILE/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe" }
$oldNodePath = $env:NODE_PATH
$server = $null; $test = $null; $state = 'failed'
try {
    if (!$env:NODE_PATH) { $env:NODE_PATH = Join-Path (Split-Path (Split-Path $node)) 'node_modules' }
    $server = Start-Process -FilePath $node -ArgumentList @(('"'+$PSScriptRoot+'/serve-web.cjs"'), ('"'+$PackageDirectory+'"'), $Port) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$work/server.log" -RedirectStandardError "$work/server.stderr.log"
    $null = $server.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        if ($server.HasExited) { throw "Owned HTTP server failed: $work/server.stderr.log" }
        $ready = (Get-Content "$work/server.log" -Raw -ErrorAction SilentlyContinue) -match "http://127.0.0.1:$Port/"
        if (!$ready) { Start-Sleep -Milliseconds 100 }
    } while (!$ready -and [DateTime]::UtcNow -lt $deadline)
    if (!$ready) { throw 'Owned HTTP server did not become ready.' }
    $test = Start-Process -FilePath $node -ArgumentList @(('"'+$PSScriptRoot+'/test-web.cjs"'), "http://127.0.0.1:$Port/", ('"'+$work+'"')) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$work/browser.log" -RedirectStandardError "$work/browser.stderr.log"
    $null = $test.Handle
    if (!$test.WaitForExit(180000)) { throw 'Headed browser interaction timed out.' }
    if ($test.ExitCode -ne 0) { throw "Headed browser interaction failed: $work/browser.stderr.log" }
    $state = 'success'
    Write-Output "runtime-manual headed Web checks PASS (visual review pending): $work"
} finally {
    try {
        if ($test -and !$test.HasExited) { Stop-AgentOwnedProcess -Process $test -Description 'script browser validation' }
        if ($server -and !$server.HasExited) { Stop-AgentOwnedProcess -Process $server -Description 'script validation HTTP server' }
    } finally {
        if ($test) { $test.Dispose() }; if ($server) { $server.Dispose() }
        $env:NODE_PATH = $oldNodePath
        Complete-HEStorageRun -Run $record -State $state
    }
}
