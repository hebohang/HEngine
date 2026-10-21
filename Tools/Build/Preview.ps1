param([Parameter(Mandatory=$true)][string]$Project, [string]$Profile = '', [switch]$OpenBrowser)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OutputDirectory.ps1')
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
$config = Get-Content -LiteralPath (Join-Path $projectRoot 'Config/BuildProfiles.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (!$Profile) { $Profile = $config.activeProfile }
$selected = @($config.profiles | Where-Object id -eq $Profile)
if ($selected.Count -ne 1) { throw 'Invalid preview profile.' }
$output = Resolve-HEBuildOutput $projectRoot $selected[0].output
Assert-HEBuildOutputOwner $output $projectRoot $Profile -RequireBuild
if ($selected[0].platform -eq 'WeChat') {
    $cli = $selected[0].devToolsPath
    if (!$cli) { $cli = $env:HE_WECHAT_CLI }
    if (!$cli) { $cli = (Get-Command wechatidecli.cmd -ErrorAction SilentlyContinue).Source }
    if (!$cli -or !(Test-Path -LiteralPath $cli)) { throw 'Set the WeChat DevTools CLI path in this profile or HE_WECHAT_CLI.' }
    & $cli open --project $output
    if ($LASTEXITCODE -ne 0) { throw 'WeChat DevTools could not open the exported project.' }
    Write-Output 'WeChat DevTools opened the exported project.'
    exit 0
}
if ($selected[0].platform -ne 'Web') { throw 'Preview is available for Web and WeChat profiles.' }
$node = $env:HE_SCRIPT_NODE
if (!$node) { $node = (Get-Command node.exe -ErrorAction SilentlyContinue).Source }
if (!$node) { $node = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' }
if (!(Test-Path -LiteralPath $node)) { throw 'Install Node.js or set HE_SCRIPT_NODE to enable local browser preview.' }
$port = [int]$selected[0].previewPort
if ($port -lt 1024 -or $port -gt 65535) { throw 'Preview port must be between 1024 and 65535.' }
$arguments = @((Join-Path $PSScriptRoot 'ServeWeb.cjs'),$output,"$port")
if ($OpenBrowser) { $arguments += '--open' }
& $node @arguments
exit $LASTEXITCODE
