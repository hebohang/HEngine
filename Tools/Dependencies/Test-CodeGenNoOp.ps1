param([string]$BuildRoot = 'out/agent/game-relwithdebinfo', [string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
. (Join-Path $PSScriptRoot '../Agent/LibClang.ps1')
Import-HeToolchain
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
$output = Join-Path $build 'GeneratedSource'
$manifest = Get-Content -Raw (Join-Path $output '.he-codegen/manifest.json') | ConvertFrom-Json
$before = @{}
foreach ($entry in $manifest.outputs) {
    $file = Join-Path $output $entry.path
    $before[$entry.path] = @((Get-HEFileDigest $file), [IO.File]::GetLastWriteTimeUtc($file).Ticks)
}
$previousPreference = $ErrorActionPreference
try {
    # Windows PowerShell wraps native stderr warnings in ErrorRecord objects.
    # Preserve diagnostics and judge the native command by its actual exit code.
    $ErrorActionPreference = 'Continue'
    & cmake --build $build --target HEngineCodeGen --config $Configuration --parallel 2 *> (Join-Path $build 'codegen/noop-test.log')
    $codegenExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousPreference }
if ($codegenExit -ne 0) { throw 'No-op codegen command failed.' }
$result = Get-Content -Raw (Join-Path $build "codegen/verify-result-$Configuration.json") | ConvertFrom-Json
if ($result.changed -ne 0 -or $result.deleted -ne 0 -or $result.status -ne 'clean') { throw 'Codegen was not clean.' }
foreach ($entry in $manifest.outputs) {
    $file = Join-Path $output $entry.path
    if ((Get-HEFileDigest $file) -ne $before[$entry.path][0] -or [IO.File]::GetLastWriteTimeUtc($file).Ticks -ne $before[$entry.path][1]) {
        throw "No-op modified output: $($entry.path)"
    }
}
@{evidence='unit-test';result='PASS';outputs=$before.Count;changed=0;deleted=0;hashAndMtimeEqual=$true;durationMs=$result.duration} | ConvertTo-Json
