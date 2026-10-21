param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture = Join-Path $repo 'out/agent/publish-tests/Independent Project'
New-Item -ItemType Directory -Force (Join-Path $fixture 'Config'), (Join-Path $fixture 'Web') | Out-Null
'{"projectId":"publish-fixture","name":"Independent Project"}' | Set-Content -LiteralPath (Join-Path $fixture 'Project.heproject') -Encoding UTF8
@'
<Project Sdk="Microsoft.NET.Sdk.WebAssembly">
  <Import Project="$(HEngineRoot)/Engine/Managed/WebAssembly.props" />
</Project>
'@ | Set-Content -LiteralPath (Join-Path $fixture 'Web/Fixture.csproj') -Encoding UTF8
@'
using System.Runtime.InteropServices.JavaScript;
[assembly: System.Runtime.Versioning.SupportedOSPlatform("browser")]
public static partial class Program {
    public static void Main() { }
    [JSExport] public static int Value() => 42;
}
'@ | Set-Content -LiteralPath (Join-Path $fixture 'Web/Program.cs') -Encoding UTF8
@'
import argparse
from pathlib import Path
parser=argparse.ArgumentParser(); parser.add_argument('--output',required=True)
output=Path(parser.parse_args().output); output.mkdir(parents=True,exist_ok=True)
(output/'index.html').write_text('<!doctype html><title>Independent engine project</title><p id="value">Loading</p><script type="module" src="main.js"></script>',encoding='utf-8')
(output/'main.js').write_text("import {dotnet} from './_framework/dotnet.js'; import {VirtualJoystick} from './engine/Input/VirtualJoystick.js'; const runtime=await dotnet.create();const exports=await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName); document.getElementById('value').textContent=String(exports.Program.Value()); globalThis.fixtureReady=typeof VirtualJoystick==='function';",encoding='utf-8')
'@ | Set-Content -LiteralPath (Join-Path $fixture 'export.py') -Encoding UTF8
$config = @{schema=1;activeProfile='web';profiles=@(@{id='web';name='Fixture';platform='Web';configuration='Release';output='Dist/Web';managedProject='Web/Fixture.csproj';contentExporter='export.py';previewPort=8771})}
$configPath = Join-Path $fixture 'Config/BuildProfiles.json'
$config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $configPath -Encoding UTF8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Publish.ps1') -Project $fixture
if ($LASTEXITCODE -ne 0) { throw 'Independent project build failed.' }
$before = (Get-FileHash -LiteralPath (Join-Path $fixture 'Dist/Web/index.html')).Hash
$checks = @()
foreach ($case in @('path-escape','unsupported-platform','missing-project-input')) {
    $saved = $config | ConvertTo-Json -Depth 8
    if ($case -eq 'path-escape') { $config.profiles[0].output='../escape' }
    if ($case -eq 'unsupported-platform') { $config.profiles[0].platform='Unknown' }
    if ($case -eq 'missing-project-input') { $config.profiles[0].managedProject='Missing.csproj' }
    $config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $configPath -Encoding UTF8
    $ErrorActionPreference = 'Continue'
    try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Publish.ps1') -Project $fixture *> (Join-Path $fixture "$case.log"); $code = $LASTEXITCODE } finally { $ErrorActionPreference = 'Stop' }
    if ($code -eq 0) { throw "Invalid configuration accepted: $case" }
    if ((Get-FileHash -LiteralPath (Join-Path $fixture 'Dist/Web/index.html')).Hash -ne $before) { throw 'Failed build damaged previous output.' }
    $checks += $case; $config = $saved | ConvertFrom-Json
}
$config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $configPath -Encoding UTF8
Write-Output "build-only PASS: independent C# Web project under a spaced path: $fixture/Dist/Web"
Write-Output "unit-test PASS: $($checks -join ', '); previous output preserved"
