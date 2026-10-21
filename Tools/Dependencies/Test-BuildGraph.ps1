param(
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [string]$Target = 'HGame',
    [string]$AbsentSourcePattern = '',
    [string]$PresentSourcePattern = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
Import-HeToolchain
$commands = @(& ninja -C $BuildRoot -t commands $Target)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Ninja target graph.' }
$sources = @($commands | ForEach-Object {
    if ($_ -match ' -c (.+)$') { $matches[1].Replace('\','/').Trim('"') }
})
if (!$sources.Count) { throw 'No compile commands found.' }
if ($AbsentSourcePattern) {
    $unexpected = @($sources | Where-Object {$_ -match $AbsentSourcePattern})
    if ($unexpected.Count) { throw "Unexpected dependencies: $($unexpected -join ', ')" }
}
if ($PresentSourcePattern -and !@($sources | Where-Object {$_ -match $PresentSourcePattern}).Count) {
    throw "Expected dependency missing: $PresentSourcePattern"
}
[pscustomobject]@{evidence='static-check';result='PASS';target=$Target;compileCommands=$sources.Count;absent=$AbsentSourcePattern;present=$PresentSourcePattern} | ConvertTo-Json
