# Process-local environment only: never changes registry or machine PATH.
function Get-HeAndroidToolchainRoot {
    if ($env:HE_ANDROID_TOOLCHAIN_ROOT) { return [IO.Path]::GetFullPath($env:HE_ANDROID_TOOLCHAIN_ROOT) }
    return [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../out/agent/android-toolchain'))
}
function Import-HeAndroidToolchain {
    $pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
    $root = Get-HeAndroidToolchainRoot
    $localDotnet = Join-Path $root "$($pin.dotnet.directory)/dotnet.exe"
    if (Test-Path -LiteralPath $localDotnet) {
        $env:DOTNET_ROOT = Split-Path $localDotnet
        $env:DOTNET_MULTILEVEL_LOOKUP = '0'
        $env:PATH = $env:DOTNET_ROOT + [IO.Path]::PathSeparator + $env:PATH
    }
    if (!$env:JAVA_HOME) { $env:JAVA_HOME = Join-Path $root "$($pin.jdk.directory)/$($pin.jdk.home)" }
    if (!$env:ANDROID_SDK_ROOT) {
        $env:ANDROID_SDK_ROOT = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $root 'sdk' }
    }
    if (!$env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME = Join-Path $root $pin.ndk.directory }
    return [pscustomobject]@{ Root=$root; Pin=$pin; Dotnet=(Get-Command dotnet -ErrorAction Stop).Source }
}

function Get-HeAndroidFileHash([string]$path, [string]$algorithm = 'SHA256') {
    $stream = [IO.File]::OpenRead($path)
    $hasher = [Security.Cryptography.HashAlgorithm]::Create($algorithm)
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
