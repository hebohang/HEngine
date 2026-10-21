param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Config = 'Config/Android.json',
    [string]$Profile = 'android',
    [string]$Template = '',
    [ValidateSet('Check','Build')][string]$Action = 'Build',
    [ValidateSet('Desktop','Portable')][string]$Quality = 'Desktop',
    [string]$OutputDirectory,
    [string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Dependencies/Environment.ps1"
Import-HEDependencyEnvironment
$precompiled = $Template -or (Test-Path -LiteralPath "$repo/HEngine.distribution.json")
. (Join-Path $PSScriptRoot 'Toolchain.ps1')
$androidToolchain = Import-HeAndroidToolchain
$toolchainLease = $null
$packageWorkspace = $null
$packageComplete = $false
$toolchainLock = Join-Path $androidToolchain.Root '.setup.lock'
if (Test-Path -LiteralPath $toolchainLock) {
    $toolchainLease = [IO.File]::Open($toolchainLock, 'Open', 'Read', 'Read')
}
try {
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
if ([IO.Path]::GetExtension($projectRoot) -eq '.heproject') { $projectRoot = Split-Path $projectRoot }
function Project-File([string]$relative) {
    if (!$relative -or [IO.Path]::IsPathRooted($relative)) { throw 'Android project paths must be relative.' }
    $path = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (!$path.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Invalid Android project file: $relative" }
    return $path
}
$configPath = Project-File $Config
$settings = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($settings.schema -ne 1 -or $settings.applicationId -notmatch '^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$') { throw 'Android applicationId must be a reverse-domain identifier.' }
if ($settings.versionCode -isnot [int] -or $settings.versionCode -lt 1 -or $settings.versionCode -gt 2100000000) { throw 'Android versionCode must be a positive integer below 2100000001.' }
if ($settings.versionName -notmatch '^\d+\.\d+\.\d+([.-][A-Za-z0-9]+)*$') { throw 'Invalid Android versionName.' }
if ($settings.minimumSdk -isnot [int] -or $settings.minimumSdk -lt 29 -or $settings.minimumSdk -gt 36) { throw 'Android minimumSdk must be between 29 and 36.' }
if ($settings.format -notin @('apk','aab') -or $settings.signing -notin @('Development','Release')) { throw 'Android format/signing must be apk/aab and Development/Release.' }
if ($settings.format -eq 'aab' -and $settings.signing -ne 'Release') { throw 'AAB distribution requires Release signing.' }
$icon = if ($settings.icon) { Project-File $settings.icon } else { '' }
$managed = Project-File $settings.managedProject
$gameConfig = Project-File $settings.gameConfig
$plugins = Project-File $settings.pluginConfig
$sdk = $env:ANDROID_SDK_ROOT
if (!$sdk) { $sdk = $env:ANDROID_HOME }
if (!$sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
$ndk = $env:ANDROID_NDK_HOME
if (!$ndk) { $ndk = Join-Path $sdk "ndk/$($settings.ndkVersion)" }
$java = $env:JAVA_HOME
if (!$java -or !(Test-Path -LiteralPath (Join-Path $java 'bin/java.exe'))) { throw 'Set JAVA_HOME to JDK 21. See Docs/Android.md.' }
$javaRelease = Join-Path $java 'release'
if (!(Test-Path -LiteralPath $javaRelease) -or (Get-Content -LiteralPath $javaRelease -Raw) -notmatch 'JAVA_VERSION="21[.]') { throw 'Android requires JDK 21. Run hectl android setup -AcceptAndroidLicenses.' }
if (!(Test-Path -LiteralPath (Join-Path $sdk 'platform-tools/adb.exe'))) { throw 'Install Android SDK platform-tools and set ANDROID_SDK_ROOT. See Docs/Android.md.' }
if (!$precompiled) {
if (!(Test-Path -LiteralPath (Join-Path $ndk 'build/cmake/android.toolchain.cmake'))) { throw "Install Android NDK $($settings.ndkVersion), or set ANDROID_NDK_HOME." }
$ndkVersion = Get-Content -LiteralPath (Join-Path $ndk 'source.properties') -Raw
if ($ndkVersion -notmatch [regex]::Escape("Pkg.Revision = $($settings.ndkVersion)")) { throw 'Android NDK does not match the project pin.' }
}
$buildTools = Get-ChildItem -LiteralPath (Join-Path $sdk 'build-tools') -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and [version]$_.Name -ge [version]'35.0.0' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$buildTools) { throw 'Install Android SDK build-tools 35.0.0 or newer (16 KB APK alignment support).' }
Push-Location (Join-Path $repo 'Engine/Managed')
try {
    $workloads = & dotnet workload list 2>&1
    if ($LASTEXITCODE -ne 0 -or ($workloads -join "`n") -notmatch '(?m)^android\s') { throw 'Install the Android workload: dotnet workload install android (from Engine/Managed).' }
} finally { Pop-Location }
if ($settings.signing -eq 'Release') {
    foreach ($name in @('HE_ANDROID_KEYSTORE','HE_ANDROID_KEY_ALIAS','HE_ANDROID_STORE_PASSWORD','HE_ANDROID_KEY_PASSWORD')) {
        if (![Environment]::GetEnvironmentVariable($name)) { throw "Release signing requires environment variable $name. No signing credentials belong in project JSON." }
    }
    if (!(Test-Path -LiteralPath $env:HE_ANDROID_KEYSTORE -PathType Leaf)) { throw 'Release keystore does not exist.' }
}
if ($precompiled) {
    $profileConfig = Get-Content -Raw -Encoding UTF8 -LiteralPath (Project-File 'Config/BuildProfiles.json') | ConvertFrom-Json
    $profileEntry = @($profileConfig.profiles | Where-Object id -eq $Profile)
    if ($profileEntry.Count -ne 1) { throw 'Select one Android profile.' }
    $profileAndroidConfig = if ($profileEntry[0].androidConfig) { $profileEntry[0].androidConfig } else { 'Config/Android.json' }
    if ((Project-File $profileAndroidConfig) -cne $configPath) { throw 'Config must match the selected Android build profile.' }
    & "$repo/Tools/Distribution/Export-Native.ps1" -Project $projectRoot -Profile $Profile -Platform Android -Quality $Quality -Template $Template -Action Check
} else {
    . (Join-Path $repo 'Tools/Agent/Toolchain.ps1')
    Import-HeToolchain
}
if ($Action -eq 'Check') {
    Write-Output "ANDROID CHECK PASS: JDK=$java; SDK=$sdk; nativeCompilation=$(!$precompiled); dotnet=$($androidToolchain.Dotnet)"
    return
}
if (!$WorkDirectory -or !$OutputDirectory) { throw 'Build via Publish.ps1 so output ownership and atomic publication are enforced.' }
$work = [IO.Path]::GetFullPath($WorkDirectory)
if (!$work.StartsWith((Join-Path $repo 'out/agent') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Android work directory must be under out/agent.' }
New-Item -ItemType Directory -Force $work,$OutputDirectory | Out-Null
function Run([string]$tool, [string[]]$arguments) {
    $previousPreference = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $tool @arguments; $nativeExit = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) { throw "Android build stage failed: $tool (exit $nativeExit)" }
}
# Host tools run on Windows; only verified cooked content crosses to Android.
if ($precompiled) {
    $hostContent = Join-Path $work ('c-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    $contentWork = Join-Path $work ('w-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    & "$repo/Tools/Distribution/Export-Native.ps1" -Project $projectRoot -Profile $Profile -Platform Android -Quality $Quality `
        -Template $Template -OutputDirectory $hostContent -WorkDirectory $contentWork
    $packagedLibrary = Join-Path $hostContent 'libhegame.so'
    $managedWork = Join-Path $contentWork 'managed'
    $managedDirectory = Join-Path $contentWork 'scripts'
} else {
$hostLog = Join-Path $work 'host-package.log'
$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Tools/Agent/Package.ps1') -Project $projectRoot -GameConfig $settings.gameConfig -PluginConfig $plugins -ScriptQuality $Quality > $hostLog 2>&1
    $hostExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousPreference }
if ($hostExit -ne 0) { throw "Host cooker/package failed. See $hostLog" }
$packageLine = Get-Content -LiteralPath $hostLog | Select-String '^Packaged game: (.+)$' | Select-Object -Last 1
if (!$packageLine) { throw 'Host packaging did not report a verified package.' }
$hostContent = Split-Path $packageLine.Matches[0].Groups[1].Value.Trim()
try {
    $ErrorActionPreference = 'Continue'
    $parserOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Tools/Agent/Build.ps1') -Target HEngineParser -Configuration Release 2>&1
    $parserExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousPreference }
if ($parserExit -ne 0) { throw 'Host parser build failed.' }
$parserOutput | ForEach-Object { Write-Output $_ }
$parserLine = $parserOutput | Select-String '^artifact: (.+)$' | Select-Object -Last 1
if (!$parserLine) { throw 'Host parser build did not report its artifact.' }
$parser = $parserLine.Matches[0].Groups[1].Value.Trim()
if (!(Test-Path -LiteralPath $parser)) { throw "Host parser was not found: $parser" }
$native = Join-Path $work 'native'
Run 'cmake' @('-S',$repo,'-B',$native,'-G','Ninja',"-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",'-DANDROID_ABI=arm64-v8a',"-DANDROID_PLATFORM=android-$($settings.minimumSdk)",'-DCMAKE_BUILD_TYPE=Release',"-DHE_PROJECT_DIR=$projectRoot","-DHE_PLUGIN_CONFIG=$plugins","-DHE_PACKAGE_GAME_CONFIG=$($settings.gameConfig)","-DHE_ANDROID_HOST_CONTENT=$hostContent","-DHE_CODEGEN_PARSER_EXECUTABLE=$parser")
Run 'cmake' @('--build',$native,'--target','HGame','-j','4')
# Keep the original ELF with symbols in native/bin for crash symbolication.
$nativePackage = Join-Path $work 'native-package'
New-Item -ItemType Directory -Force $nativePackage | Out-Null
$packagedLibrary = Join-Path $nativePackage 'libhegame.so'
Copy-Item -LiteralPath (Join-Path $native 'bin/libhegame.so') -Destination $packagedLibrary -Force
Run (Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe') @('--strip-unneeded',$packagedLibrary)
$managedWork = Join-Path $work 'managed'
Run 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'Tools/Scripting/BuildManaged.ps1'),'-Project',$projectRoot,'-WorkDirectory',$managedWork,'-OutputDirectory',(Join-Path $managedWork 'runtime'),'-Platform','Android','-Quality',$Quality,'-ManagedProject',$settings.managedProject)
$managedDirectory = Join-Path $managedWork 'runtime'
}
# The host cook uses Windows scripts, while Android can declare additional
# dependencies behind platform defines. Never publish an incomplete APK.
& (Join-Path $PSScriptRoot 'VerifyScriptDependencies.ps1') -Project $projectRoot -Metadata (Join-Path $managedDirectory 'GameScripts.metadata.json') -Manifest (Join-Path $hostContent 'Game.manifest.json')
$assets = Join-Path $work ('assets-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $assets | Out-Null
foreach ($name in @('Game.pak','Game.manifest.json','LICENSE.txt','THIRD_PARTY_NOTICES.txt','Licenses')) {
    $source = Join-Path $hostContent $name
    if (!(Test-Path -LiteralPath $source)) { throw "Verified host package is missing $name" }
    Copy-Item -LiteralPath $source -Destination $assets -Recurse
}
$assetFiles = @(Get-ChildItem -LiteralPath $assets -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($assets.Length+1).Replace('\','/');sha256=(Get-HeAndroidFileHash $_.FullName 'SHA256')}
})
$sha = [Security.Cryptography.SHA256]::Create()
try { $identity = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes((($assetFiles | ForEach-Object { "$($_.path):$($_.sha256)`n" }) -join '')))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
[ordered]@{schema=1;identity=$identity;files=$assetFiles} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $assets 'android-content.json') -Encoding UTF8
$descriptor = Get-Content -LiteralPath (Project-File 'Project.heproject') -Raw -Encoding UTF8 | ConvertFrom-Json
$androidToolchain = Import-HeAndroidToolchain
. (Join-Path $PSScriptRoot 'PackageWorkspace.ps1')
$packageWorkspace = New-HeAndroidPackageWorkspace $androidToolchain.Root
$packageRoot = $packageWorkspace.Path
Write-Output "Android managed packaging workspace: $packageRoot"
[ordered]@{schema=1;path=$packageRoot;purpose='Android managed packaging; retained on failure'} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $work 'package-workspace.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $repo 'Engine/Source/Runtime/Platform/Android/Managed') -Destination (Join-Path $packageRoot 'host') -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'Engine/Managed/global.json') -Destination $packageRoot
Copy-Item -LiteralPath $assets -Destination (Join-Path $packageRoot 'assets') -Recurse
Copy-Item -LiteralPath $managedDirectory -Destination (Join-Path $packageRoot 'scripts') -Recurse
Copy-Item -LiteralPath $packagedLibrary -Destination (Join-Path $packageRoot 'libhegame.so')
if ($icon) {
    Copy-Item -LiteralPath $icon -Destination (Join-Path $packageRoot 'game_icon.png')
    $icon = Join-Path $packageRoot 'game_icon.png'
}
$publishOutput = Join-Path $packageRoot 'publish'
$arguments = @('publish',(Join-Path $packageRoot 'host/HEngine.Android.csproj'),'-c','Release','--artifacts-path',(Join-Path $packageRoot 'dotnet'),'-o',$publishOutput,
    "-p:HEAndroidIcon=$icon","-p:HEAndroidManagedDirectory=$packageRoot/scripts","-p:HEPlatform=Android","-p:HEQuality=$Quality", "-p:HEAndroidApplicationId=$($settings.applicationId)","-p:HEAndroidDisplayName=$($descriptor.DisplayName)","-p:HEAndroidVersionCode=$($settings.versionCode)","-p:HEAndroidVersionName=$($settings.versionName)","-p:SupportedOSPlatformVersion=$($settings.minimumSdk).0","-p:HEAndroidAssets=$packageRoot/assets","-p:HEAndroidNativeLibrary=$packageRoot/libhegame.so","-p:AndroidSdkDirectory=$sdk","-p:JavaSdkDirectory=$java","-p:AndroidPackageFormats=$($settings.format)","-p:HEAndroidReleaseSigning=$($settings.signing -eq 'Release')")
Push-Location $packageRoot
try {
    # A fresh workspace also avoids obsolete incremental JNI proxy classes.
    Run 'dotnet' $arguments
} finally { Pop-Location }
$artifact = Get-ChildItem -LiteralPath $publishOutput -File -Filter "*-Signed.$($settings.format)" | Select-Object -First 1
if (!$artifact) { throw 'Android publish did not produce a signed artifact.' }
if ($settings.format -eq 'apk') {
    Run (Join-Path $buildTools.FullName 'apksigner.bat') @('verify','--verbose',$artifact.FullName)
    Run (Join-Path $buildTools.FullName 'zipalign.exe') @('-c','-P','16','4',$artifact.FullName)
    & (Join-Path $PSScriptRoot 'VerifyManifest.ps1') -Apk $artifact.FullName -Aapt2 (Join-Path $buildTools.FullName 'aapt2.exe') -ApplicationId $settings.applicationId -DisplayName $descriptor.DisplayName -VersionCode $settings.versionCode -MinimumSdk $settings.minimumSdk
}
if ($settings.format -eq 'aab') {
    $verification = & (Join-Path $java 'bin/jarsigner.exe') '-J-Duser.language=en' -verify $artifact.FullName 2>&1
    if ($LASTEXITCODE -ne 0 -or ($verification -join "`n") -notmatch 'jar verified[.]') { throw 'AAB signature verification failed.' }
}
$python = if ($env:HE_BUILD_PYTHON) { $env:HE_BUILD_PYTHON } else { 'py.exe' }
$nativeHash = Get-HeAndroidFileHash $packagedLibrary 'SHA256'
$verifyArgs = @((Join-Path $PSScriptRoot 'VerifyPackage.py'),$artifact.FullName,'--native-sha256',$nativeHash)
$playerFingerprint = ''
if ($precompiled) {
    $playerFingerprint = (Get-Content -Raw -Encoding UTF8 -LiteralPath "$hostContent/Game.manifest.json" | ConvertFrom-Json).Player.RuntimeFingerprint
    $verifyArgs += @('--runtime-fingerprint',$playerFingerprint)
}
if ($python -eq 'py.exe') { $verifyArgs = @('-3.11') + $verifyArgs }
Run $python $verifyArgs
Copy-Item -LiteralPath $artifact.FullName -Destination $OutputDirectory
[ordered]@{schema=1;applicationId=$settings.applicationId;versionCode=$settings.versionCode;format=$settings.format;signing=$settings.signing;renderer='Vulkan';abi='arm64-v8a';artifact=$artifact.Name;sha256=(Get-HeAndroidFileHash $artifact.FullName 'SHA256');contentIdentity=$identity;precompiledPlayer=[bool]$precompiled;nativeSha256=$nativeHash;runtimeFingerprint=$playerFingerprint} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'android-build.json') -Encoding UTF8
$packageComplete = $true
Write-Output "ANDROID BUILD PASS ($($settings.signing) signing): $(Join-Path $OutputDirectory $artifact.Name)"

} finally {
    if ($toolchainLease) { $toolchainLease.Dispose() }
    if ($packageComplete -and $packageWorkspace) { Remove-HeAndroidPackageWorkspace $packageWorkspace }
}
