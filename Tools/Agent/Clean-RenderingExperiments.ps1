param([switch]$Apply, [string]$Python = 'python', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo ('out/agent/rendering-evidence/cleanup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$planPath = Join-Path $OutputDirectory 'plan.json'
& $Python (Join-Path $PSScriptRoot 'rendering_cleanup.py') --output $planPath
if ($LASTEXITCODE -ne 0) { throw 'Cleanup dependency audit failed.' }
$plan = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8 | ConvertFrom-Json
$root = [IO.Path]::GetFullPath((Join-Path $repo 'Project/Assets/RenderingBenchmarks'))
function Assert-Target([string]$Relative) {
    $absolute = [IO.Path]::GetFullPath((Join-Path $repo $Relative))
    if (!$absolute.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Outside cleanup root: $Relative" }
    $item = Get-Item -LiteralPath $absolute -Force
    for ($cursor = $item; $cursor -and $cursor.FullName.Length -ge $root.Length; $cursor = $cursor.Parent) {
        if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point in cleanup path: $absolute" }
        if ($cursor -is [IO.FileInfo]) { $cursor = $cursor.Directory; if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point: $absolute" } }
    }
    return $absolute
}
# Verify the entire inventory before deleting any file.
foreach ($record in @($plan.candidates) + @($plan.retained)) {
    $path = Assert-Target $record.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $record.sha256) { throw "File changed during audit: $path" }
}
if (!$Apply) { Write-Output 'Preview only. Run with -Apply to audit again and delete listed experiments.'; return }
$deletedBytes = [long]0
$deletedCount = 0
$tracked = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
& git -C $repo ls-files | ForEach-Object { $null = $tracked.Add($_) }
if ($LASTEXITCODE -ne 0) { throw 'Could not recheck Git tracking state.' }
foreach ($record in $plan.candidates) {
    $path = Assert-Target $record.path
    if ($tracked.Contains($record.path)) { throw "File became tracked; refusing deletion: $path" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $record.sha256) { throw "File changed before deletion: $path" }
    Remove-Item -LiteralPath $path -Force
    $deletedBytes += $record.bytes; $deletedCount++
}
# Empty directories alone may be removed; never recursively delete a variant directory.
Get-ChildItem -LiteralPath $root -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object {
    $null = Assert-Target ($_.FullName.Substring($repo.Length + 1))
    if (!(Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $_.FullName }
}
foreach ($record in $plan.retained) {
    $path = Assert-Target $record.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $record.sha256) { throw "Retained resource changed: $path" }
}
[ordered]@{result='PASS';deletedFiles=$deletedCount;deletedLogicalBytes=$deletedBytes;retainedFiles=@($plan.retained).Count;retainedHashesUnchanged=$true;plan=$planPath} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
Get-Content -LiteralPath (Join-Path $OutputDirectory 'result.json')
