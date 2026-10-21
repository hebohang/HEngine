param([Parameter(Mandatory=$true)][string]$BuildRoot, [string]$Name = 'BuildFeatures', [int]$Parallel = 4)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
Import-HeToolchain
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
if (!$build.StartsWith((Join-Path $repo 'out/agent/') , [StringComparison]::OrdinalIgnoreCase)) { throw 'Acceptance build must be inside out/agent.' }
$cache = Get-Content -Raw (Join-Path $build 'CMakeCache.txt')
if ($cache -notmatch '(?m)^HE_BUILD_DEV_GAME:BOOL=ON\r?$') { throw 'Only a development Game acceptance cache may be reused.' }
$artifact = Join-Path $build 'bin/HEngineTests.exe'
$process = $null
try {
    & cmake -S $repo -B $build -DHE_BUILD_TESTS=ON -DHE_CODEGEN_HOST_OVERRIDE=Tests *> (Join-Path $build 'feature-tests-config.log')
    if ($LASTEXITCODE -ne 0) { throw 'Tests configure failed.' }
    & cmake --build $build --target HEngineTests --parallel $Parallel *> (Join-Path $build 'feature-tests-build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Tests build failed.' }
    $stdout = Join-Path $build "feature-tests-$Name.stdout.log"
    $stderr = Join-Path $build "feature-tests-$Name.stderr.log"
    $process = Start-Process -FilePath $artifact -ArgumentList @('--run', $Name) -WorkingDirectory $repo -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Owned feature test timed out.' }
    $process.Refresh()
    if ($process.ExitCode -ne 0) { throw "Feature test failed; see $stdout and $stderr" }
    Write-Output "unit-test PASS: HEngineTests --run $Name ($build)"
}
finally {
    if ($null -ne $process) { if (!$process.HasExited) {$process.Kill()}; $process.Dispose() }
    & cmake -S $repo -B $build -DHE_BUILD_TESTS=OFF -DHE_CODEGEN_HOST_OVERRIDE=DevGame *> (Join-Path $build 'feature-tests-restore.log')
    if ($LASTEXITCODE -ne 0) { throw 'Failed to restore Game host after acceptance.' }
}
