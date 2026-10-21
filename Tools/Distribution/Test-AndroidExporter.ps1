param(
    [Parameter(Mandatory=$true)][string]$Engine,
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Profile = 'android'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Engine).Path
if (!(Test-Path -LiteralPath "$root/HEngine.distribution.json")) { throw 'Validate Android export from a binary installation.' }
foreach ($forbidden in @('CMakeLists.txt','Engine/Source/Runtime/Launch','Engine/Source/Game','Project')) {
    if (Test-Path -LiteralPath "$root/$forbidden") { throw "Source-free fixture contains $forbidden" }
}
. "$root/Tools/Agent/Project.ps1"
. "$root/Tools/Distribution/Template.ps1"
$info = Resolve-HEProject $Project $root
$template = Get-HEExportTemplate $root 'Android-arm64'
$oldPath = $env:PATH
$oldNdk = $env:ANDROID_NDK_HOME
$logRoot = "$root/out/agent/android-validation/$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $logRoot -Force
try {
    $env:PATH = ($oldPath.Split(';') | Where-Object { $_ -notmatch '(?i)Visual Studio|VC[\\/]Tools|android-ndk' }) -join ';'
    $env:ANDROID_NDK_HOME = "$logRoot/no-ndk-installed"
    $oldErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/Tools/Build/Publish.ps1" `
            -Project $info.Root -Profile $Profile -Action Build *> "$logRoot/export.log"
        $exportExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldErrorPreference }
    if ($exportExit -ne 0) { throw "Source-free Android export failed; see $logRoot/export.log" }
    $publish = Get-Content -Raw -Encoding UTF8 -LiteralPath "$($info.Root)/Saved/Build/$Profile.json" | ConvertFrom-Json
    if (!$publish.success) { throw 'Android export has no successful publication receipt.' }
    $receipt = Get-Content -Raw -Encoding UTF8 -LiteralPath "$($publish.output)/android-build.json" | ConvertFrom-Json
    $expected = (Get-FileHash -LiteralPath $template.Files[$template.Manifest.Player]).Hash.ToLowerInvariant()
    if ($receipt.precompiledPlayer -ne $true -or $receipt.nativeSha256 -cne $expected -or
        $receipt.runtimeFingerprint -cne $template.Manifest.RuntimeFingerprint) { throw 'Android export did not reuse the installed native template.' }
    $artifact = Join-Path $publish.output $receipt.artifact
    if ((Get-FileHash -LiteralPath $artifact).Hash.ToLowerInvariant() -cne $receipt.sha256) { throw 'Published Android artifact hash changed.' }
    [ordered]@{schema=1;evidence='test-layer';pass=$true;engine=$root;project=$info.Root;output=$artifact;
        nativeSha256=$expected;nativeCompilation=$false;ndkAvailable=$false;
        validation='Managed build/cook, actual APK/AAB native hash and content checks, signing and alignment; no device gameplay claim.'} |
        ConvertTo-Json | Set-Content -LiteralPath "$logRoot/result.json" -Encoding UTF8
    Write-Output "Source-free Android export PASS: $logRoot/result.json"
} finally { $env:PATH=$oldPath; $env:ANDROID_NDK_HOME=$oldNdk }
