param([string]$Cli = $env:HE_WECHAT_CLI, [string]$OutputDirectory = '', [switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$project = [IO.Path]::GetFullPath((Join-Path $repo 'out/agent/scripting/game-wechat'))
if (!$Cli) { $Cli = (Get-Command wechatide.cmd -ErrorAction SilentlyContinue).Source }

if (!$OutputDirectory) { $OutputDirectory = "$repo/out/agent/scripting/wechat-preview-$(Get-Date -Format yyyyMMdd-HHmmss-fff)" }
$work = [IO.Path]::GetFullPath($OutputDirectory)
if ($work.Equals($project,[StringComparison]::OrdinalIgnoreCase) -or $work.StartsWith($project+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Preview evidence must be outside the exported project.'
}
function Verify-PreviewInput {
    if ((Get-Item -LiteralPath $project).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Preview project is a redirected path.' }
    $manifestPath = Join-Path $project 'game-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $null = $known.Add('game-manifest.json')
    foreach ($entry in $manifest.assets) {
        $file = [IO.Path]::GetFullPath((Join-Path $project $entry.path))
        if (!$file.StartsWith($project+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Preview manifest path escape.' }
        $item = Get-Item -LiteralPath $file
        if ($item.PSIsContainer -or $item.Length -ne $entry.bytes -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Preview input differs from verified export: $($entry.path)"
        }
        if (!$known.Add($entry.path.Replace('\','/'))) { throw 'Duplicate preview manifest entry.' }
    }
    foreach ($item in Get-ChildItem -LiteralPath $project -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Preview input contains a redirected path.' }
        $relative = $item.FullName.Substring($project.Length+1).Replace('\','/')
        if (!$item.PSIsContainer -and !$known.Contains($relative)) { throw "Undeclared preview file: $relative" }
    }
    return $manifest
}
function Invoke-PreviewTool([string]$Name,[string[]]$Arguments,[string]$Receipt) {
    $previous = $ErrorActionPreference
    try { $ErrorActionPreference='Continue'; & $Cli -c Codex $Name --project $project @Arguments *> "$work/$Receipt.log"; $code=$LASTEXITCODE }
    finally { $ErrorActionPreference=$previous }
    $text = Get-Content "$work/$Receipt.log" -Raw
    $start = $text.IndexOf('{')
    if ($code -ne 0 -or $start -lt 0) { throw "Preview operation failed: $Receipt; see its log." }
    $value = $text.Substring($start) | ConvertFrom-Json
    if (!$value.ok -or $value.result.success -eq $false) { throw "Preview operation rejected: $Receipt; see its log." }
    return $value.result
}
$manifest = Verify-PreviewInput
if ($VerifyOnly) { Write-Output "Verified preview export: $($manifest.buildId)"; return }
if (!$Cli -or !(Test-Path -LiteralPath $Cli)) { throw 'Pass -Cli or set HE_WECHAT_CLI to the installed wechatide.cmd.' }
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'scripting-wechat-preview' -Paths @($work) -Outer
$state='failed'; $opened=$false
$receipt=[ordered]@{status='FAIL';buildId=$manifest.buildId;managedIdentity=$manifest.managedIdentity;
    gameplaySourceIdentity=$manifest.gameplaySourceIdentity;deviceRuntime='NOT_TESTED'}
try {
    $null=Invoke-PreviewTool open_project_window @('--window-mode','liteMode') 'open'
    $opened=$true
    $preview=Invoke-PreviewTool create_preview_qrcode @('--qr-format','image','--qr-output',"$work/preview.png") 'preview'
    if (!(Test-Path "$work/preview.png" -PathType Leaf)) { throw 'DevTools did not write a preview image.' }
    $current=Verify-PreviewInput
    if ($current.buildId -ne $manifest.buildId) { throw 'Export changed during preview preparation.' }
    $receipt.status='PREVIEW_READY'
    $receipt.devToolsReportedSize=$preview.info.size
    $receipt.sizeScope='DevTools preview info.size values; not evidence of compressed network transfer bytes.'
    $receipt.qrImage="$work/preview.png"
    $receipt.qrSha256=(Get-FileHash "$work/preview.png" -Algorithm SHA256).Hash.ToLowerInvariant()
    $state='success'
} catch { $receipt.error=$_.Exception.Message; throw }
finally {
    try { if ($opened) { $null=Invoke-PreviewTool close_project_window @() 'close' }; $receipt.cleanup='PASS' }
    catch { $receipt.cleanup='FAILED'; $state='unknown'; throw }
    finally {
        $receipt | ConvertTo-Json -Depth 10 | Set-Content "$work/result.json" -Encoding UTF8
        Complete-HEStorageRun -Run $record -State $state
    }
}
Write-Output "WeChat preview ready (device runtime unverified): $work/preview.png"
