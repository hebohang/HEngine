$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$work = Join-Path $repo ('out/agent/scripting/native-web-baseline-' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'native-web-baseline' -Paths @($work) -Outer
$state = 'failed'
try {
    & "$repo/hectl.bat" wechat export *> "$work/build.log"
    if ($LASTEXITCODE -ne 0) { throw "Native baseline build/export failed: $work/build.log" }
    $node = $env:HE_SCRIPT_NODE
    if (!$node) { $node = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' }
    & $node "$PSScriptRoot/export-native-web-baseline.cjs" "$work/web" *> "$work/export.json"
    if ($LASTEXITCODE -ne 0) { throw "Native Web baseline export failed: $work/export.json" }
    $state = 'success'
    Write-Output "build-only native Web baseline PASS: $work/web/browser-test.html"
} finally { Complete-HEStorageRun -Run $record -State $state }
