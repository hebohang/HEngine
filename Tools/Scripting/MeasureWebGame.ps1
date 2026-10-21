param([string]$OutputDirectory = '', [ValidateRange(1,65535)][int]$Port = 8772, [switch]$Trace)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo ('out/agent/scripting/web-performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$busy = @(Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('cl.exe','link.exe','ninja.exe','MSBuild.exe','cmake.exe') -or ($_.Name -match '^python.*[.]exe$' -and $_.CommandLine -match 'storage[.]py\s+(auto|stale)')})
if ($busy.Count) { throw 'A build or storage cleanup process is active. Run performance measurement after it finishes.' }
$node = $env:HE_SCRIPT_NODE
if (!$node) { $node = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' }
if (!(Test-Path -LiteralPath $node)) { $node = (Get-Command node.exe -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$run = Start-HEStorageRun -Project "$repo/Project" -Workflow 'scripting-web-performance' -Paths @($OutputDirectory) -Outer
$state = 'failed'
$previousNodePath = $env:NODE_PATH
try {
    if (!$env:NODE_PATH) { $env:NODE_PATH = Join-Path (Split-Path (Split-Path $node)) 'node_modules' }
    $measureArguments = @("$PSScriptRoot/MeasureWebGame.cjs", $OutputDirectory, [string]$Port)
    if ($Trace) { $measureArguments += '--trace' }
    & $node @measureArguments *> "$OutputDirectory/run.log"
    if ($LASTEXITCODE -ne 0) { throw "Browser measurement failed; see $OutputDirectory/run.log" }
    $state = 'success'
} finally {
    if ($null -eq $previousNodePath) { Remove-Item Env:NODE_PATH -ErrorAction SilentlyContinue } else { $env:NODE_PATH=$previousNodePath }
    Complete-HEStorageRun -Run $run -State $state
}
Write-Host "Measured real Web game: $OutputDirectory"
