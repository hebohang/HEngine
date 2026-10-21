param([ValidateSet('Web','WeChat')][string]$Platform = 'Web')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$work = Join-Path $repo ("out/agent/scripting/portable-modules-$Platform-" + (Get-Date -Format yyyyMMdd-HHmmss-fff))
$project = Join-Path $work 'project'
$stage = Join-Path $work 'stage'
$node = "$env:USERPROFILE/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe"
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$run = Start-HEStorageRun -Project "$repo/Project" -Workflow 'portable-script-modules' -Paths @($work) -Outer
$state = 'failed'
function Write-Module([string]$Id, [string]$Category, [string[]]$Platforms, [string[]]$References, [string]$Code) {
    $directory = Join-Path $project "Scripts/$Id"
    New-Item -ItemType Directory -Force $directory | Out-Null
    [IO.File]::WriteAllText("$directory/Code.cs", $Code)
    @{schema=1;id=$Id;assembly="Fixture.$Id";category=$Category;includePlatforms=$Platforms;references=$References;sources=@("Scripts/$Id")} |
        ConvertTo-Json | Set-Content "$directory/module.hemodule" -Encoding UTF8
}
try {
    Write-Module 'shared' 'Runtime' @('Web','WeChat') @() 'public static class SharedModule { public static float Step(double speed, double seconds) => (float)(speed * System.Math.Clamp(seconds, 0, 0.05)); }'
    $source = [IO.File]::ReadAllText("$repo/Project/Scripts/CSharp/ScriptTouchSample.cs")
    $source = $source.Replace('(float)(Speed * Math.Clamp(seconds, 0, 0.05))', 'SharedModule.Step(Speed, seconds)')
    if (!$source.Contains('SharedModule.Step')) { throw 'Sample fixture no longer contains the expected movement expression.' }
    Write-Module 'gameplay' 'Runtime' @('Web','WeChat') @('shared') $source
    Write-Module 'desktop' 'Runtime' @('Windows') @() '#error Platform exclusion failed'
    Write-Module 'editor' 'Editor' @('Web','WeChat') @() '#error Editor category leaked'
    & "$PSScriptRoot/BuildManaged.ps1" -Project $project -WorkDirectory $stage -OutputDirectory "$stage/output" -Platform $Platform -Quality Portable -IsolatedCompiler *> "$work/build.log"
    $receipt = Get-Content "$stage/output/build.json" -Raw | ConvertFrom-Json
    $metadata = Get-Content "$stage/registry/GameScripts.metadata.json" -Raw | ConvertFrom-Json
    if ($receipt.platform -ne $Platform -or ($metadata.modules.id -join '|') -ne 'shared|gameplay' -or
        $metadata.types[0].moduleId -ne 'gameplay' -or $metadata.types[0].portableDiagnostics.Count) { throw 'Module platform/category/portable registry contract failed.' }
    Push-Location "$repo/Engine/Managed"
    try {
        & dotnet publish HEngine.ScriptWasm/HEngine.ScriptWasm.csproj -c Release "-p:HEPlatform=$Platform" -p:HEQuality=Portable `
            --artifacts-path "$stage/wasm-artifacts" "-p:ScriptRegistryDir=$stage/registry" "-p:GeneratedApiDir=$stage/api" `
            "-p:ModuleReferencesFile=$stage/modules/ModuleReferences.props" "-p:GameScriptsProject=$stage/modules/GameScripts.csproj" `
            -o "$work/managed" *> "$work/wasm.log"
        if ($LASTEXITCODE -ne 0) { throw 'Module Wasm publish failed.' }
    } finally { Pop-Location }
    & $node "$PSScriptRoot/export-web.cjs" "$work/managed" "$repo/out/agent/web-script-release" "$stage/registry" "$work/package" *> "$work/export.log"
    if ($LASTEXITCODE -ne 0) { throw 'Real module export failed.' }
    $port = if ($Platform -eq 'Web') { 8773 } else { 8774 }
    & "$PSScriptRoot/TestWebGame.ps1" -PackageDirectory "$work/package" -EvidenceDirectory "$work/browser" -Port $port *> "$work/browser.log"
    # A real call through the module reference must propagate an unsupported API.
    [IO.File]::WriteAllText("$project/Scripts/shared/Code.cs", 'public static class SharedModule { public static float Step(double speed, double seconds) => float.Parse(System.IO.File.ReadAllText("never-read.txt")); }')
    & "$PSScriptRoot/BuildManaged.ps1" -Project $project -WorkDirectory $stage -OutputDirectory "$stage/output" -Platform $Platform -Quality Portable -IsolatedCompiler *> "$work/rejected-build.log"
    $metadata = Get-Content "$stage/registry/GameScripts.metadata.json" -Raw | ConvertFrom-Json
    if (($metadata.types[0].portableDiagnostics -join '|') -notmatch 'SharedModule.Step.*System.IO.File.ReadAllText') { throw 'Cross-module unsupported API escaped audit.' }
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $node "$PSScriptRoot/export-web.cjs" "$work/managed" "$repo/out/agent/web-script-release" "$stage/registry" "$work/rejected" *> "$work/rejected-export.log"
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($code -eq 0 -or (Test-Path "$work/rejected") -or (Get-Content "$work/rejected-export.log" -Raw) -notmatch 'Unsupported portable API.*SharedModule.Step') { throw 'Unsupported module graph was not rejected before publication.' }
    $state = 'success'
    @{status='PASS';platform=$Platform;moduleRuntime='browser-wasm';checks=@('runtime-only modules','platform exclusion','explicit merged registration','real cross-module gameplay','transitive API rejection','no partial publication');evidence=$work} |
        ConvertTo-Json | Set-Content "$work/result.json" -Encoding UTF8
    Write-Output "unit-test + headed browser portable modules PASS: $work"
} finally { Complete-HEStorageRun -Run $run -State $state }
