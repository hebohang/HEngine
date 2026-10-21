# Shared selection contract for CMake, standalone codegen and dependency checks.
function Get-HEFileDigest {
    param([string]$Path, [string]$Algorithm = 'SHA256')
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.HashAlgorithm]::Create($Algorithm)
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function Resolve-HELibClang {
    param([string]$Root = '', [switch]$SkipHashValidation)
    if (!$Root) { $Root = $env:HE_CODEGEN_LIBCLANG_ROOT }
    if (!$Root) { $Root = Join-Path $PSScriptRoot '../../Engine/Source/Programs/HEngineParser/3rd_party/LLVM/Win64' }
    $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $manifestPath = Join-Path $Root 'manifest.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "libclang SDK manifest is missing: $manifestPath. Use the bundled SDK or an explicitly prepared, versioned SDK; system LLVM is not selected automatically."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.platform -ne 'win-x64' -or
        $manifest.version -notmatch '^\d+\.\d+\.\d+$' -or [version]$manifest.version -lt [version]'20.0.0' -or
        $manifest.resourceVersion -notmatch '^\d+(\.\d+)*$') { throw 'Unsupported libclang SDK manifest.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        $relative = [string]$file.path
        if (!$relative -or [IO.Path]::IsPathRooted($relative) -or $relative.Contains(':') -or
            $relative.Contains('\') -or ($relative.Split('/') -contains '..') -or !$seen.Add($relative) -or
            $file.sha256 -notmatch '^[0-9a-f]{64}$') { throw "Invalid libclang manifest entry: $relative" }
        $path = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Incomplete libclang SDK: $path" }
        if (!$SkipHashValidation -and (Get-HEFileDigest $path) -ine $file.sha256) {
            throw "libclang SDK checksum mismatch: $path"
        }
    }
    foreach ($required in @('bin/libclang.dll','lib/libclang.lib','include/clang-c/Index.h','LICENSE.txt',
        "lib/clang/$($manifest.resourceVersion)/include/stddef.h")) {
        if (!$seen.Contains($required)) { throw "libclang manifest does not cover $required" }
    }
    [pscustomobject]@{
        Root=$Root.Replace('\','/'); Version=[string]$manifest.version; Major=([version]$manifest.version).Major
        IncludeRoot=($Root + '/include').Replace('\','/')
        ResourceRoot=($Root + '/lib/clang/' + $manifest.resourceVersion).Replace('\','/')
    }
}
