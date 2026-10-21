param([string]$Root = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../PackageWorkspace.ps1')
$oldRoot = $env:HE_ANDROID_BUILD_ROOT
$testRoot = Join-Path ([IO.Path]::GetFullPath($Root)) ('he-android-test-' + [guid]::NewGuid().ToString('N'))
$workspace = $null
try {
    $env:HE_ANDROID_BUILD_ROOT = $testRoot
    $workspace = New-HeAndroidPackageWorkspace $testRoot
    if (!(Test-Path -LiteralPath "$($workspace.Path)/.he-android-workspace")) { throw 'Missing ownership marker.' }
    $forged = [pscustomobject]@{Root=(Join-Path $testRoot 'other');Path=$workspace.Path}
    $rejected = $false
    try { Remove-HeAndroidPackageWorkspace $forged } catch { $rejected=$true }
    if (!$rejected -or !(Test-Path -LiteralPath $workspace.Path)) { throw 'Forged ownership was not rejected.' }
    foreach ($invalid in @((Join-Path $testRoot ([char]0x6e38 + 'game')), ('C:\' + ('a' * 100)), '\\server\share\build')) {
        $env:HE_ANDROID_BUILD_ROOT = $invalid
        $rejected = $false
        try { $null = New-HeAndroidPackageWorkspace $testRoot } catch { $rejected=$true }
        if (!$rejected) { throw 'Unsupported AAPT2 path was accepted.' }
    }
    Remove-HeAndroidPackageWorkspace $workspace
    if (Test-Path -LiteralPath $workspace.Path) { throw 'Owned workspace was not removed.' }
    $workspace = $null
    Write-Output 'Android workspace PASS: ASCII allocation, ownership guard, Unicode/long/UNC rejection, cleanup.'
} finally {
    $env:HE_ANDROID_BUILD_ROOT = $oldRoot
    if ($workspace -and (Test-Path -LiteralPath $workspace.Path)) { Remove-HeAndroidPackageWorkspace $workspace }
    # Non-recursive: never remove unexpected evidence or unrelated contents.
    if ((Test-Path -LiteralPath $testRoot) -and !(Get-ChildItem -LiteralPath $testRoot -Force)) { Remove-Item -LiteralPath $testRoot }
}
