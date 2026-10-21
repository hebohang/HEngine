param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($Directory)
$package = Get-Content -LiteralPath (Join-Path $root 'package-files.json') -Raw | ConvertFrom-Json
$receipt = Get-Content -LiteralPath (Join-Path $root 'build.json') -Raw | ConvertFrom-Json
$pinned = & (Join-Path $PSScriptRoot 'GetPinnedRuntime.ps1')
if ($package.schema -ne 1 -or !$receipt.shipping -or $package.archiveSha512 -ne $pinned.Manifest.archiveSha512) { throw 'Invalid C# shipping package identity.' }
$expected = @{}
foreach ($file in $pinned.Manifest.files) { $expected['Runtime/' + $file.path] = $file.sha256 }
$required = @('GameScripts.dll','GameScripts.metadata.json','HEngine.ScriptCore.dll','HEngine.ScriptRuntime.dll','HEngine.ScriptRuntime.deps.json','HEngine.ScriptRuntime.runtimeconfig.json')
. (Join-Path $PSScriptRoot 'ModulePackageContract.ps1')
$required += @(Get-HEModulePackageFiles $receipt)
if ((@($receipt.managedFiles.path | Sort-Object) -join '|') -ne (($required | Sort-Object) -join '|')) { throw 'Invalid managed assembly package list.' }
foreach ($file in $receipt.managedFiles) { $expected[$file.path] = $file.sha256 }
$allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $package.files) {
    $name = [string]$file.path
    if ($name.Contains('\') -or $name.Contains(':') -or $name.StartsWith('/') -or ($name.Split('/') -contains '..')) { throw 'Unsafe C# package manifest path.' }
    $path = [IO.Path]::GetFullPath((Join-Path $root $name))
    if (!$path.StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or !$allowed.Add($path)) { throw 'Duplicate or escaping C# package file.' }
    if ($expected.ContainsKey($name)) {
        if ($expected[$name] -ne $file.sha256) { throw "C# package differs from the pinned runtime/build receipt: $name" }
        $expected.Remove($name)
    } elseif ($name -notin @('build.json','Runtime/runtime-manifest.json')) { throw "Unapproved C# package file: $name" }
    $stream = [IO.File]::OpenRead($path); $hash = [Security.Cryptography.SHA256]::Create()
    try { $actual = [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
    if ($actual -ne $file.sha256) { throw "C# package file integrity mismatch: $name" }
}
if ($expected.Count) { throw 'C# package is missing required runtime/assembly files.' }
$all = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'C# package root is a link.' }
foreach ($file in $all) {
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'C# package contains a link.' }
    if (!$file.PSIsContainer -and $file.Name -ne 'package-files.json' -and !$allowed.Contains($file.FullName)) { throw "C# package contains an unlisted file: $($file.Name)" }
}
if (@($all | Where-Object { !$_.PSIsContainer }).Count -ne $allowed.Count+1) { throw 'C# package file count differs from its manifest.' }
return [pscustomobject]@{Files=@($allowed);Runtime=$pinned.Manifest.version}
