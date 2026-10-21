param([string]$BuildRoot = 'out/agent/game-relwithdebinfo', [string]$Project = 'Project')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
Import-HeToolchain
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
$id = [guid]::NewGuid().ToString('N')
$header = Join-Path $projectRoot "Source/DependencyDiscoveryProbe_$id.h"
$retired = Join-Path $repo "out/agent/dependency-validation/discovery-$id.h"
if (!(Test-Path -LiteralPath (Split-Path $header)) -or (Test-Path -LiteralPath $header)) { throw 'Unsafe discovery fixture path.' }
if (![IO.Path]::GetFullPath($header).StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or ![IO.Path]::GetFullPath($retired).StartsWith($repo + '\out\agent\dependency-validation\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture outside allowed paths.' }
function Invoke-Generate($Phase) {
    & cmake --build $build --target HEngineCodeGen --parallel 2 *> (Join-Path $build "codegen/discovery-$Phase.log")
    if ($LASTEXITCODE -ne 0) { throw "Discovery $Phase generation failed." }
}
try {
    [IO.File]::WriteAllText($header, "#pragma once`n")
    Invoke-Generate 'plain'
    [IO.File]::WriteAllText($header, @'
#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"
namespace HEngine {
REFLECTION_TYPE(DependencyDiscoveryProbe)
STRUCT(DependencyDiscoveryProbe, Fields) {
    REFLECTION_BODY(DependencyDiscoveryProbe);
    int Value = 7;
};
}
'@)
    Invoke-Generate 'reflected'
    $manifest = Get-Content -Raw (Join-Path $build 'GeneratedSource/.he-codegen/manifest.json') | ConvertFrom-Json
    if (!@($manifest.types | Where-Object {$_.qualifiedName -eq 'HEngine::DependencyDiscoveryProbe'}).Count) { throw 'Existing header reflection marker was not discovered.' }
}
finally {
    if (Test-Path -LiteralPath $header) { Move-Item -LiteralPath $header -Destination $retired }
    Invoke-Generate 'restored'
}
$manifest = Get-Content -Raw (Join-Path $build 'GeneratedSource/.he-codegen/manifest.json') | ConvertFrom-Json
if (@($manifest.types | Where-Object {$_.qualifiedName -eq 'HEngine::DependencyDiscoveryProbe'}).Count) { throw 'Removed fixture type remains in manifest.' }
Write-Output 'unit-test PASS: existing-header marker discovery and owned stale output cleanup.'
