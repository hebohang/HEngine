param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$package = (Resolve-Path $PackageDirectory).Path
$work = Join-Path $repo ('out/agent/scripting/native-web-validation-' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'native-web-validation' -Paths @($work) -Outer
$state = 'failed'
$oldNodePath = $env:NODE_PATH; $oldHeaded = $env:HE_WECHAT_HEADED
try {
    $node = $env:HE_SCRIPT_NODE
    if (!$node) { $node = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' }
    if (!$env:NODE_PATH) { $env:NODE_PATH = Join-Path (Split-Path (Split-Path $node)) 'node_modules' }
    $env:HE_WECHAT_HEADED = '1'
    & $node "$repo/Tools/WeChat/browser_check.cjs" $package "$work/browser" *> "$work/browser.log"
    if ($LASTEXITCODE -ne 0) { throw "Native browser interaction failed: $work/browser.log" }
    & $node "$PSScriptRoot/MeasurePortableSizes.cjs" "$work/sizes.json" $package *> "$work/sizes.log"
    if ($LASTEXITCODE -ne 0) { throw "Package identity/size verification failed: $work/sizes.log" }
    $state = 'success'
    Write-Output "runtime-manual headed native Web interactions PASS (visual review pending): $work"
} finally {
    $env:NODE_PATH=$oldNodePath; $env:HE_WECHAT_HEADED=$oldHeaded
    Complete-HEStorageRun -Run $record -State $state
}
