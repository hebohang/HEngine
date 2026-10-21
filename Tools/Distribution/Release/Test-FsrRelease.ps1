param([Parameter(Mandatory=$true)][string]$BuildRoot,
    [Parameter(Mandatory=$true)][ValidateSet('x64','arm64')][string]$Architecture,
    [Parameter(Mandatory=$true)][string]$Evidence)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
. "$repo/Tools/Agent/Toolchain.ps1"
Import-HeToolchain -Toolchain (Get-HeVisualStudio -Architecture $Architecture)
if ((Get-HeWindowsHostArchitecture) -ne $Architecture) { throw 'FSR release runtime verification requires a native host.' }
$previousPreference=$ErrorActionPreference
try {
    $ErrorActionPreference='Continue'
    & cmake --build $BuildRoot --target HEngineFsrSmoke --parallel 2
    $buildExit=$LASTEXITCODE
} finally { $ErrorActionPreference=$previousPreference }
if ($buildExit -ne 0) { throw 'FSR validation program build failed.' }
$binary=Join-Path $BuildRoot 'bin/Programs/HEngineFsrSmoke/HEngineFsrSmoke.exe'
& $env:HE_BUILD_PYTHON "$repo/Tools/Dependencies/prepare_warp_test.py" --architecture $Architecture `
    --root "$repo/out/agent/dependencies/warp-validation" --target-directory (Split-Path $binary)
if ($LASTEXITCODE -ne 0) { throw 'Pinned test-only WARP preparation failed.' }
$Evidence=[IO.Path]::GetFullPath($Evidence)
$null=New-Item -ItemType Directory -Force $Evidence
$process=$null
$previousPath=$env:Path
try {
    # Editor-hosted tests also link Editor's Embree importer dependency. Use the
    # same built app-local dependencies; FSR itself still loads beside the EXE.
    $env:Path="$(Join-Path $BuildRoot 'bin');$previousPath"
    $process=Start-Process -FilePath $binary -ArgumentList @('--dx12-only','--warp','--compact',"--fsr-evidence=`"$Evidence`"",
        "--project=`"$repo/Engine/Templates/Blank`"","--engine-content=`"$repo/Engine/Content`"",
        "--generated-content=`"$BuildRoot/GeneratedContent/Engine`"") `
        -WorkingDirectory (Split-Path $binary) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$Evidence/stdout.log" -RedirectStandardError "$Evidence/stderr.log"
    $null=$process.Handle
    if (!$process.WaitForExit(300000)) { throw 'FSR WARP smoke exceeded 300 seconds.' }
    $process.WaitForExit()
    $process.Refresh()
    if ($process.ExitCode -ne 0 -or !(Select-String -LiteralPath "$Evidence/stdout.log" -Pattern '^FSR_SMOKE_PASS:' -Quiet)) {
        Get-Content "$Evidence/stderr.log" -Tail 20
        Get-Content "$Evidence/stdout.log" -Tail 20
        throw "FSR native dispatch/readback verification failed (exit $($process.ExitCode))."
    }
    Write-Output 'test-layer PASS: native FSR DX12 WARP dispatch, pixel readback, history reset and resize.'
} finally {
    $env:Path=$previousPath
    if ($process) {
        $process.Refresh()
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
