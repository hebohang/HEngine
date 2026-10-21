param(
    [Parameter(Mandatory=$true)][string]$Apk,
    [Parameter(Mandatory=$true)][string]$Aapt2,
    [Parameter(Mandatory=$true)][string]$ApplicationId,
    [Parameter(Mandatory=$true)][string]$DisplayName,
    [Parameter(Mandatory=$true)][int]$VersionCode,
    [Parameter(Mandatory=$true)][int]$MinimumSdk
)
$ErrorActionPreference = 'Stop'
$oldEncoding = [Console]::OutputEncoding
try {
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$badging = (& $Aapt2 dump badging $Apk) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot read APK manifest.' }
if ($badging -notmatch "(?m)^package: name='$([regex]::Escape($ApplicationId))' versionCode='$VersionCode'") { throw 'APK application identity/version mismatch.' }
if ($badging -notmatch "(?m)^(minSdkVersion|sdkVersion):'$MinimumSdk'") { throw 'APK minimum SDK mismatch.' }
if ($badging -notmatch "(?m)^application: label='(.*?)' icon='([^']*)'") { throw 'APK has no application label/icon.' }
if ($Matches[1] -ne $DisplayName) { throw 'APK application display name mismatch.' }
if ($badging -notmatch "(?m)^launchable-activity: name='org[.]hengine[.]runtime[.]GameActivity'") { throw 'APK has no HEngine launch activity.' }
$tree = (& $Aapt2 dump xmltree $Apk --file AndroidManifest.xml) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect APK activity configuration.' }
# Framework icons have no APK-local filename in badging. Require a real
# application icon resource in the binary manifest for both default/custom icons.
if ($tree -notmatch '(?ms)E: application[^\r\n]*\r?\n(?<attributes>(?:[^\r\n]*A: [^\r\n]*\r?\n)+)' -or
    $Matches['attributes'] -notmatch ':icon\(0x01010002\)=@0x(?!00000000)[0-9a-fA-F]{8}\b') { throw 'APK application has no icon resource.' }
if ($tree -notmatch 'screenOrientation[^\r\n]*=6\b') { throw 'APK must request sensorLandscape.' }
if ($tree -match 'debuggable[^\r\n]*=true') { throw 'Shipping APK must not be debuggable.' }
Write-Output 'MANIFEST PASS: application identity, label, icon, launch activity, landscape, minimum SDK, non-debuggable'
} finally { [Console]::OutputEncoding = $oldEncoding }
