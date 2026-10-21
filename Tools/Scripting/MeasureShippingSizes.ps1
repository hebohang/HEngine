param(
    [Parameter(Mandatory=$true)][string[]]$Summaries,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$profiles = @()
foreach ($summaryPath in $Summaries) {
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if ($summary.status -ne 'success') { throw "Unsuccessful shipping evidence: $summaryPath" }
    foreach ($entry in $summary.results) {
        if ($entry.publication -ne 'PASS' -or $entry.dependencyClosure -ne 'PASS') { throw 'Missing verified publication evidence.' }
        $exe = $entry.executable
        if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.executableSha256) { throw "Published executable changed: $exe" }
        $package = Split-Path $exe
        $manifest = Get-Content -LiteralPath (Join-Path $package 'Game.manifest.json') -Raw | ConvertFrom-Json
        if ($manifest.ContentHash -ne $entry.contentHash) { throw 'Published content identity differs from its run receipt.' }
        $files = @(Get-ChildItem -LiteralPath $package -Recurse -File)
        $csharpFiles = @($files | Where-Object {$_.FullName.StartsWith((Join-Path $package 'Scripting\CSharp') + '\',[StringComparison]::OrdinalIgnoreCase)})
        $profiles += [ordered]@{profile=$entry.profile;package=$package;executableSha256=$entry.executableSha256;
            rawFileBytes=($files | Measure-Object Length -Sum).Sum;fileCount=$files.Count;
            executableBytes=(Get-Item -LiteralPath $exe).Length;pakBytes=(Get-Item -LiteralPath (Join-Path $package 'Game.pak')).Length;
            cookedStoredBytes=($manifest.Files | Measure-Object StoredSize -Sum).Sum;
            csharpRuntimeFilesBytes=$(if ($csharpFiles.Count) { ($csharpFiles | Measure-Object Length -Sum).Sum } else { 0 })}
    }
}
$baseline = @($profiles | Where-Object {$_.profile -eq 'none'})
if ($baseline.Count -ne 1) { throw 'Exactly one verified NativeScript/both-plugins-off baseline is required.' }
foreach ($entry in $profiles) { $entry.rawFileDeltaFromNativeBytes=$entry.rawFileBytes-$baseline[0].rawFileBytes }
@{measurement='Raw published file bytes, not compressed download size or NTFS allocation. Same project/toolchain and native render pipeline; profile scenes differ by script components, with cooked-byte and pak-byte differences reported explicitly. No mobile size inference.';
    profiles=$profiles} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Output
Write-Host "Verified shipping file sizes: $Output"
