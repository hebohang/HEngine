param([switch]$AcceptLicenses, [switch]$NativeCompiler)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Toolchain.ps1')
$root = Get-HeAndroidToolchainRoot
$installNativeCompiler = $NativeCompiler -or !(Test-Path -LiteralPath (Join-Path $PSScriptRoot '../../HEngine.distribution.json'))
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
if (!$AcceptLicenses) { throw 'Android SDK installation requires license acceptance. Read Docs/Android.md, then run hectl android setup -AcceptAndroidLicenses.' }
New-Item -ItemType Directory -Force $root | Out-Null
$lease = [IO.File]::Open((Join-Path $root '.setup.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
$previousTemporary = @{}
try {
    # Workload extraction can exceed several GB. Keep it on the selected
    # toolchain volume instead of silently exhausting the system TEMP drive.
    foreach ($name in @('TEMP','TMP','NUGET_HTTP_CACHE_PATH','NUGET_PACKAGES')) {
        $previousTemporary[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        $cachePath = Join-Path $root $(if ($name -in @('TEMP','TMP')) { 'temp' } else { $name.ToLowerInvariant() })
        $null = New-Item -ItemType Directory -Path $cachePath -Force
        [Environment]::SetEnvironmentVariable($name, $cachePath, 'Process')
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    function Download($package) {
        $archive = Join-Path $root $package.archive
        if (!(Test-Path -LiteralPath $archive) -or (Get-HeAndroidFileHash $archive $package.algorithm) -ne $package.hash) {
            Write-Host "Downloading $($package.archive)"
            & curl.exe -L --fail --retry 3 --continue-at - --connect-timeout 30 --max-time 900 --speed-limit 1024 --speed-time 60 --silent --show-error $package.url -o ($archive + '.partial')
            if ($LASTEXITCODE -ne 0) { throw "Download failed: $($package.archive)" }
            if ((Get-HeAndroidFileHash ($archive + '.partial') $package.algorithm) -ne $package.hash) { throw "Toolchain checksum mismatch: $($package.archive)" }
            Move-Item -LiteralPath ($archive + '.partial') -Destination $archive -Force
        }
        return $archive
    }
    function Install-Zip($package, [string]$destination, [string]$archiveRoot, [string]$sentinel) {
        if (Test-Path -LiteralPath (Join-Path $destination $sentinel)) { return }
        $archive = Download $package
        $stage = Join-Path $root ('extract-' + [Guid]::NewGuid().ToString('N'))
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $stage)
        $source = if ($archiveRoot) { Join-Path $stage $archiveRoot } else { $stage }
        $boundary = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
        foreach ($path in @($source,$destination)) {
            if (![IO.Path]::GetFullPath($path).StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Toolchain installation escaped its cache.' }
        }
        if (!(Test-Path -LiteralPath (Join-Path $source $sentinel))) { throw "Incomplete archive: $($package.archive)" }
        if (Test-Path -LiteralPath $destination) { throw "Incomplete existing toolchain directory: $destination. Preserve it for diagnosis before retrying." }
        New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
        Move-Item -LiteralPath $source -Destination $destination
    }
    Install-Zip $pin.dotnet (Join-Path $root $pin.dotnet.directory) '' 'dotnet.exe'
    $installerRoot = Join-Path $root "$($pin.dotnet.directory)/metadata/workloads"
    if (Test-Path -LiteralPath $installerRoot) {
        $msiMarkers = @(Get-ChildItem -Path "$installerRoot/*/installertype/msi" -File -ErrorAction SilentlyContinue)
        if ($msiMarkers.Count) {
            throw 'Android setup requires the portable ZIP .NET SDK, not a copy of an MSI-managed installation. Use a fresh toolchain root or the documented external-toolchain workflow; setup will not request system installation privileges.'
        }
    }
    Install-Zip $pin.jdk (Join-Path $root $pin.jdk.directory) '' "$($pin.jdk.home)/bin/java.exe"
    if ($installNativeCompiler) {
        Install-Zip $pin.ndk (Join-Path $root $pin.ndk.directory) $pin.ndk.directory 'build/cmake/android.toolchain.cmake'
    }
    Install-Zip $pin.commandLineTools (Join-Path $root "sdk/cmdline-tools/$($pin.commandLineTools.version)") 'cmdline-tools' 'bin/sdkmanager.bat'
    # Setup always installs its managed toolchain. Build may use explicit external paths.
    $env:JAVA_HOME = Join-Path $root "$($pin.jdk.directory)/$($pin.jdk.home)"
    $env:ANDROID_SDK_ROOT = Join-Path $root 'sdk'
    $env:ANDROID_NDK_HOME = Join-Path $root $pin.ndk.directory
    $toolchain = Import-HeAndroidToolchain
    Write-Host 'Installing pinned .NET Android workload into the portable SDK'
    & $toolchain.Dotnet workload install android --skip-manifest-update --disable-parallel
    if ($LASTEXITCODE -ne 0) { throw '.NET Android workload setup failed.' }
    if (!(Test-Path -LiteralPath (Join-Path $root "$($pin.dotnet.directory)/packs/Microsoft.Android.Sdk.Windows/$($pin.androidWorkload)/tools/Xamarin.Android.Build.Tasks.dll"))) {
        throw 'The installed Android workload does not match toolchain.lock.json.'
    }
    Write-Host 'Installing Android SDK packages and accepting their licenses as requested'
    $python = if ($env:HE_BUILD_PYTHON) { $env:HE_BUILD_PYTHON } else { 'py.exe' }
    $sdkArguments = @((Join-Path $PSScriptRoot 'InstallSdk.py'),'--root',$root,'--java-home',$env:JAVA_HOME,
        '--sdk',$env:ANDROID_SDK_ROOT,'--accept-licenses')
    if ($python -eq 'py.exe') { $sdkArguments = @('-3.11') + $sdkArguments }
    & $python @sdkArguments
    if ($LASTEXITCODE -ne 0) { throw 'Android SDK setup failed.' }
    foreach ($required in @("sdk/platforms/$($pin.platform)/android.jar", "sdk/build-tools/$($pin.buildTools)/aapt2.exe", 'sdk/platform-tools/adb.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $root $required))) { throw "SDK installation is incomplete: $required" }
    }
    [ordered]@{schema=1;nativeCompiler=[bool]$installNativeCompiler;lockSha256=(Get-HeAndroidFileHash (Join-Path $PSScriptRoot 'toolchain.lock.json') 'SHA256');installed=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'installed.json') -Encoding UTF8
    Write-Host "ANDROID SETUP PASS: $root"
} finally {
    foreach ($entry in $previousTemporary.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
    $lease.Dispose()
}
