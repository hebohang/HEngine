param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$DestinationDirectory
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($SourceDirectory)
$destination = [IO.Path]::GetFullPath($DestinationDirectory)
if ($source -eq $destination -or $destination.StartsWith($source + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Script package staging must be separate from its source.' }
function FileHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path); $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function SafeFile([string]$Root, [string]$Name) {
    if ($Name.Contains('\') -or $Name.Contains(':') -or $Name.StartsWith('/') -or ($Name.Split('/') -contains '..')) { throw "Unsafe scripting package path: $Name" }
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Name))
    if (!$path.StartsWith($Root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Scripting package path escaped its root.' }
    $check = $path
    while ($check.StartsWith($Root,[StringComparison]::OrdinalIgnoreCase)) {
        if ((Test-Path -LiteralPath $check) -and ((Get-Item -LiteralPath $check -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Scripting package path contains a link.' }
        $parent = Split-Path $check
        if (!$parent -or $parent -eq $check) { break }; $check = $parent
    }
    return $path
}
foreach ($root in @($source,$destination)) {
    $path = $root
    while ($path) {
        if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Scripting package directories must not contain links.' }
        $parent = Split-Path $path
        if ($parent -eq $path) { break }; $path = $parent
    }
}
$receipt = Get-Content -LiteralPath (Join-Path $source 'build.json') -Raw | ConvertFrom-Json
if (!$receipt.shipping -or $receipt.runtime -ne '10.0.10' -or !$receipt.managedFiles) { throw 'C# package requires a verified Shipping managed build.' }
$runtimeManifest = Get-Content -LiteralPath (Join-Path $source 'Runtime/runtime-manifest.json') -Raw | ConvertFrom-Json
$lockName = if ($receipt.platform -eq 'macOS') { 'runtime.macos-arm64.lock.json' } elseif ($runtimeManifest.rid -eq 'win-arm64') { 'runtime.win-arm64.lock.json' } else { 'runtime.lock.json' }
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot $lockName) -Raw | ConvertFrom-Json
if ($runtimeManifest.archiveSha512 -ne $lock.sha512 -or $runtimeManifest.version -ne $lock.version -or $runtimeManifest.rid -ne $lock.rid) { throw 'Runtime manifest does not match the distribution lock.' }
$required = @('GameScripts.dll','GameScripts.metadata.json','HEngine.ScriptCore.dll','HEngine.ScriptRuntime.dll','HEngine.ScriptRuntime.deps.json','HEngine.ScriptRuntime.runtimeconfig.json')
. (Join-Path $PSScriptRoot 'ModulePackageContract.ps1')
$required += @(Get-HEModulePackageFiles $receipt)
if ((@($receipt.managedFiles.path | Sort-Object) -join '|') -ne (($required | Sort-Object) -join '|')) { throw 'Managed package file list differs from the supported host contract.' }
$files = @($receipt.managedFiles)
$files += @($runtimeManifest.files | ForEach-Object { [pscustomobject]@{path=('Runtime/' + $_.path);sha256=$_.sha256} })
foreach ($name in @('build.json','Runtime/runtime-manifest.json')) { $files += [pscustomobject]@{path=$name;sha256=(FileHash (Join-Path $source $name))} }
$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $files) {
    if (!$names.Add($file.path) -or $file.sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid or duplicate scripting package manifest entry.' }
    $path = SafeFile $source $file.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -or (FileHash $path) -ne $file.sha256) { throw "Scripting package integrity mismatch: $($file.path)" }
}
$actualRuntime = @(Get-ChildItem -LiteralPath (Join-Path $source 'Runtime') -Recurse -File -Force)
if ($actualRuntime.Count -ne ($runtimeManifest.files.Count + 1)) { throw 'Runtime output contains unlisted files.' }
if (Test-Path -LiteralPath $destination) {
    $previousPath = Join-Path $destination 'package-files.json'
    $existing = @(Get-ChildItem -LiteralPath $destination -Force)
    if ($existing.Count -and !(Test-Path -LiteralPath $previousPath -PathType Leaf)) { throw 'Refusing to overwrite an unowned scripting staging directory.' }
    if (Test-Path -LiteralPath $previousPath) {
        $previous = Get-Content -LiteralPath $previousPath -Raw | ConvertFrom-Json
        foreach ($file in $previous.files) {
            $path = SafeFile $destination $file.path
            if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (FileHash $path) -ne $file.sha256) { throw "Previously staged script file was modified: $($file.path)" }
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $destination -Recurse -File -Force) {
        $relative = $file.FullName.Substring($destination.Length+1).Replace('\','/')
        if ($relative -ne 'package-files.json' -and !$names.Contains($relative)) { throw "Script staging contains an unexpected file: $relative" }
    }
}
foreach ($file in $files) {
    $target = SafeFile $destination $file.path
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath (SafeFile $source $file.path) -Destination $target -Force
    if ((FileHash $target) -ne $file.sha256) { throw "Script staging copy verification failed: $($file.path)" }
}
[ordered]@{schema=1;runtime=$lock.version;archiveSha512=$lock.sha512;files=@($files | Sort-Object path)} | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $destination 'package-files.json') -Encoding UTF8
Write-Host "C# package staging PASS: $($files.Count) verified files; $destination"
