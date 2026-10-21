$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$work = Join-Path $repo ('out/agent/scripting/editor-compiler-contract-' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record = Start-HEStorageRun -Project "$repo/Project" -Workflow 'editor-compiler-contract' -Paths @($work) -Outer
$state = 'failed'
try {
    & "$PSScriptRoot/TestProcessCapture.ps1" -OutputDirectory "$work/process"
    $runner = "$work/process/ProcessCaptureTests.exe"
    $shell = (Get-Command powershell.exe).Source
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $runner run $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PSScriptRoot/CompileEditor.ps1" -Project "$repo/Project" *> "$work/positive.log"
        $positiveCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($positiveCode -ne 0 -or (Get-Content "$work/positive.log" -Raw) -notmatch 'Compile PASS') { throw "Real compiler success path failed: $work/positive.log" }
    $fixture = "$work/project/Scripts/CSharp"
    New-Item -ItemType Directory -Force $fixture | Out-Null
    Copy-Item "$repo/Project/Scripts/CSharp/packages.lock.json" -Destination $fixture
    $projectFile = "$fixture/GameScripts.csproj"
    $xml = '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="' +
        [Security.SecurityElement]::Escape("$repo/Engine/Managed/HEngine.ScriptCore/HEngine.ScriptCore.csproj") + '" /></ItemGroup></Project>'
    Set-Content $projectFile $xml -Encoding UTF8
    Set-Content "$fixture/ExpectedCompilerFailure.cs" 'public class Broken { this is deliberately invalid C#; }' -Encoding UTF8
    try {
        $ErrorActionPreference = 'Continue'
        & $runner run $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PSScriptRoot/CompileEditor.ps1" -Project "$work/project" *> "$work/negative.log"
        $negativeCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($negativeCode -eq 0 -or (Get-Content "$work/negative.log" -Raw) -notmatch 'ExpectedCompilerFailure.cs.*error CS') { throw "Compiler syntax diagnostic/exit code lost: $work/negative.log" }
    '{"schema":1,"id":"fixture.broken","assembly":"Fixture.Broken","sources":["Scripts/CSharp"]}' | Set-Content "$fixture/broken.hemodule" -Encoding UTF8
    try {
        $ErrorActionPreference = 'Continue'
        & $runner run $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PSScriptRoot/CompileEditor.ps1" -Project "$work/project" *> "$work/module-negative.log"
        $moduleCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    $moduleOutput = Get-Content "$work/module-negative.log" -Raw
    if ($moduleCode -eq 0 -or $moduleOutput -notmatch 'HE_SCRIPT_DIAGNOSTICS=([^\r\n]+)') { throw 'Missing failed module compiler receipt.' }
    $receipt = Get-Content $matches[1] -Raw | ConvertFrom-Json
    $moduleDiagnostic = @($receipt.diagnostics | Where-Object { $_.module -eq 'fixture.broken' -and $_.code -like 'CS*' -and $_.line -gt 0 -and $_.column -gt 0 })
    if (!$moduleDiagnostic.Count -or !(Test-Path $moduleDiagnostic[0].log) -or !@($receipt.modules | Where-Object { $_.id -eq 'fixture.broken' -and $_.status -eq 'failed' }).Count) { throw 'Module identity/location/log/status lost in editor receipt.' }
    $state = 'success'
    Write-Output "unit-test real editor compiler process PASS: $work"
} finally { Complete-HEStorageRun -Run $record -State $state }
