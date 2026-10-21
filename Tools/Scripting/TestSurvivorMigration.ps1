param(
    [string]$Python = '',
    [string]$Node = '',
    [string]$NodeModules = ''
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$work = Join-Path $repo 'out/agent/survivor-parity'
$bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies'
if (!$Python) {
    $Python = $env:HE_WECHAT_PYTHON
    if (!$Python -and (Test-Path (Join-Path $bundled 'python/python.exe'))) { $Python = Join-Path $bundled 'python/python.exe' }
    if (!$Python) { $Python = (Get-Command python.exe -ErrorAction Stop).Source }
}
if (!$Node) {
    $Node = $env:HE_SCRIPT_NODE
    if (!$Node -and (Test-Path (Join-Path $bundled 'node/bin/node.exe'))) { $Node = Join-Path $bundled 'node/bin/node.exe' }
    if (!$Node) { $Node = (Get-Command node.exe -ErrorAction Stop).Source }
}
if (!$NodeModules -and (Test-Path (Join-Path $bundled 'node/node_modules/playwright'))) {
    $NodeModules = Join-Path $bundled 'node/node_modules'
}
$oldNodePath = $env:NODE_PATH
Push-Location $repo
try {
    if ($NodeModules) { $env:NODE_PATH = $NodeModules }
    & $Python (Join-Path $PSScriptRoot 'AuditSurvivorMigration.py')
    if ($LASTEXITCODE -ne 0) { throw 'Migration inventory failed.' }
    & $Python (Join-Path $PSScriptRoot 'TestSurvivorParity.py')
    if ($LASTEXITCODE -ne 0) { throw 'Desktop differential rules failed.' }
    Push-Location (Join-Path $repo 'Engine/Managed')
    try {
        & dotnet publish (Join-Path $PSScriptRoot 'SurvivorParityWeb/SurvivorParityWeb.csproj') -c Release `
            --artifacts-path (Join-Path $work 'web-artifacts') -o (Join-Path $work 'web-publish') *> (Join-Path $work 'web-build.log')
        if ($LASTEXITCODE -ne 0) { throw "Wasm build failed: $work/web-build.log" }
    } finally { Pop-Location }
    & $Node (Join-Path $PSScriptRoot 'TestSurvivorWebParity.cjs')
    if ($LASTEXITCODE -ne 0) { throw 'Browser Wasm differential rules failed.' }
    Write-Output 'unit-test PASS: desktop and browser Wasm rules. Host gameplay acceptance is checked separately.'
} finally {
    $env:NODE_PATH = $oldNodePath
    Pop-Location
}
