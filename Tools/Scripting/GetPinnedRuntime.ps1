param([string]$CacheDirectory, [string]$RuntimeDirectory = $env:HE_SCRIPT_RUNTIME_ROOT,
      [ValidateSet('', 'x64', 'arm64')][string]$Architecture = '',
      [ValidateSet('Windows','macOS')][string]$Platform = $(if (($PSVersionTable.PSEdition -eq 'Core') -and $IsMacOS) { 'macOS' } else { 'Windows' }))
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$CacheDirectory) { $CacheDirectory = Join-Path $repo 'out/agent/scripting/deps' }
$cache = [IO.Path]::GetFullPath($CacheDirectory)
if ($Platform -eq 'macOS') {
    . (Join-Path $PSScriptRoot '../Dependencies/Environment.ps1')
    Import-HEDependencyEnvironment
    $result = & $env:HE_BUILD_PYTHON (Join-Path $repo 'Tools/macOS/PinnedRuntime.py') --lock (Join-Path $PSScriptRoot 'runtime.macos-arm64.lock.json') --cache $cache
    if ($LASTEXITCODE -ne 0) { throw 'Pinned macOS .NET runtime acquisition failed.' }
    return ($result | ConvertFrom-Json)
}
. (Join-Path $PSScriptRoot '../Agent/Architecture.ps1')
if (!$Architecture -and (Test-Path -LiteralPath "$repo/HEngine.distribution.json")) {
    $sdkPlatform = (Get-Content -LiteralPath "$repo/HEngine.distribution.json" -Raw | ConvertFrom-Json).Platform
    if ($sdkPlatform -notin @('Windows-x64','Windows-arm64')) { throw 'Unsupported SDK runtime architecture.' }
    $Architecture = $sdkPlatform.Substring('Windows-'.Length)
}
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$lockFile = if ($Architecture -eq 'x64') { 'runtime.lock.json' } else { 'runtime.win-arm64.lock.json' }
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot $lockFile) -Raw | ConvertFrom-Json
if ($lock.schema -ne 1 -or $lock.version -ne '10.0.10' -or $lock.rid -ne "win-$Architecture" -or $lock.sha512 -notmatch '^[0-9a-f]{128}$') { throw 'Unsupported scripting runtime lock.' }
# A source-build environment override must not make an installed SDK depend on
# a different installation. Explicit diagnostic arguments remain available.
if ((Test-Path -LiteralPath "$repo/HEngine.distribution.json") -and !$PSBoundParameters.ContainsKey('RuntimeDirectory')) { $RuntimeDirectory = '' }
# Binary SDKs already contain the locked runtime. Validate and reuse it so an
# offline export does not download another copy into a source-build cache.
if ($RuntimeDirectory -or (!$PSBoundParameters.ContainsKey('CacheDirectory') -and (Test-Path -LiteralPath "$repo/HEngine.distribution.json"))) {
    . "$repo/Tools/Distribution/Template.ps1"
    $root = if ($RuntimeDirectory) { [IO.Path]::GetFullPath($RuntimeDirectory) } else { Join-Path $repo 'Scripting/CSharp/Runtime' }
    $manifest = Get-Content -Raw -LiteralPath (Resolve-HEExportTemplateFile $root 'runtime-manifest.json') | ConvertFrom-Json
    if ($manifest.archiveSha512 -cne $lock.sha512 -or $manifest.version -ne $lock.version -or
        $manifest.rid -ne $lock.rid -or !$manifest.files -or @($manifest.files).Count -gt 10000) {
        throw 'Bundled .NET runtime does not match runtime.lock.json.'
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        if (!$names.Add($file.path) -or $file.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid bundled runtime inventory.' }
        $path = Resolve-HEExportTemplateFile $root $file.path
        if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $file.sha256) { throw "Bundled runtime was modified: $($file.path)" }
    }
    foreach ($required in @('host/fxr/10.0.10/hostfxr.dll','shared/Microsoft.NETCore.App/10.0.10/coreclr.dll','shared/Microsoft.NETCore.App/10.0.10/System.Private.CoreLib.dll','LICENSE.txt','ThirdPartyNotices.txt')) {
        if (!$names.Contains($required)) { throw "Bundled runtime is missing $required" }
    }
    if (@(Get-ChildItem -LiteralPath $root -Recurse -File -Force).Count -ne ($names.Count + 1)) { throw 'Bundled runtime contains unlisted files.' }
    return [pscustomobject]@{Root=$root;Manifest=$manifest}
}
New-Item -ItemType Directory -Force $cache | Out-Null
$guard = [IO.File]::Open((Join-Path $cache 'runtime-acquisition.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
function Get-RuntimeDigest([string]$Path, [string]$Algorithm) {
    $stream = [IO.File]::OpenRead($Path); $hash = [Security.Cryptography.HashAlgorithm]::Create($Algorithm)
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
try {
    $archive = Join-Path $cache ('dotnet-runtime-' + $lock.sha512 + '.zip')
    if (!(Test-Path -LiteralPath $archive)) {
        $partial = $archive + '.partial'
        & curl.exe --fail --location --retry 2 --max-time 300 $lock.url --output $partial
        if ($LASTEXITCODE -ne 0) { throw 'Pinned .NET runtime download failed.' }
        if ((Get-RuntimeDigest $partial 'SHA512') -ne $lock.sha512) { throw 'Downloaded .NET runtime SHA-512 differs from the lock.' }
        Move-Item -LiteralPath $partial -Destination $archive
    }
    if ((Get-RuntimeDigest $archive 'SHA512') -ne $lock.sha512) { throw 'Cached .NET archive is corrupt; it was not extracted.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    $root = Join-Path $cache ('runtime-' + $lock.sha512.Substring(0,16))
    New-Item -ItemType Directory -Force $root | Out-Null
    $files = @(); $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\','/')
            if ($name.StartsWith('/') -or $name.Contains(':') -or ($name.Split('/') -contains '..')) { throw "Unsafe runtime archive entry: $name" }
            if (!$entry.Name) { continue }
            if (!$names.Add($name)) { throw "Duplicate runtime archive entry: $name" }
            # The native game is the executable host; don't distribute dotnet.exe.
            if ($name -notmatch '^(host/fxr/10[.]0[.]10/|shared/Microsoft[.]NETCore[.]App/10[.]0[.]10/|LICENSE[.]txt$|ThirdPartyNotices[.]txt$)') { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $root $name))
            if (!$target.StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime extraction escaped its cache.' }
            $parent = Split-Path $target
            New-Item -ItemType Directory -Force $parent | Out-Null
            $check = Get-Item -LiteralPath $parent
            while ($check -and $check.FullName.StartsWith($cache,[StringComparison]::OrdinalIgnoreCase)) {
                if ($check.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Runtime cache cannot contain junctions or symlinks.' }
                $check = $check.Parent
            }
            if ((Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime cache file is a link.' }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
            $files += [ordered]@{path=$name;sha256=(Get-RuntimeDigest $target 'SHA256')}
        }
    } finally { $zip.Dispose() }
    foreach ($required in @('host/fxr/10.0.10/hostfxr.dll','shared/Microsoft.NETCore.App/10.0.10/coreclr.dll','shared/Microsoft.NETCore.App/10.0.10/System.Private.CoreLib.dll','LICENSE.txt','ThirdPartyNotices.txt')) {
        if (!($files.path -contains $required)) { throw "Pinned runtime archive is missing $required" }
    }
    $manifest = [ordered]@{version=$lock.version;rid=$lock.rid;source=$lock.url;archiveSha512=$lock.sha512;files=@($files | Sort-Object path)}
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'runtime-manifest.json') -Encoding UTF8
    return [pscustomobject]@{Root=$root;Manifest=$manifest}
} finally { $guard.Dispose() }
