param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$ValidationRoot,
    [string]$Template = '',
    [switch]$SkipAssembly
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$PSScriptRoot/Template.ps1"
$platform = if ($SkipAssembly) { Get-HEWindowsTemplatePlatform $ValidationRoot } else { Get-HEWindowsTemplatePlatform $repo }
. "$repo/Tools/Agent/Project.ps1"
if (!$Template) {
    $Template = if ($SkipAssembly) { Join-Path $ValidationRoot "ExportTemplates/$platform" }
        else { "$repo/out/agent/export-templates/$platform" }
}
$payload = Get-HEExportTemplate $repo $platform $Template
$projectInfo = Resolve-HEProject $Project $repo
$root = [IO.Path]::GetFullPath($ValidationRoot)
if ((Test-Path -LiteralPath $root) -and !$SkipAssembly) { throw 'Use a fresh validation root, or explicitly reuse an assembled fixture with -SkipAssembly.' }
if ($root.StartsWith($repo + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'The validation installation must be outside the source checkout.'
}
$null = New-Item -ItemType Directory -Path $root -Force
function Copy-TestTree([string]$Source, [string]$Destination, [switch]$NoNative) {
    $null = New-Item -ItemType Directory -Path $Destination -Force
    $excluded = @('*.pdb','*.ilk','*.pyc')
    if ($NoNative) { $excluded += @('*.cpp','*.h','CMakeLists.txt') }
    & robocopy.exe $Source $Destination /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD obj __pycache__ .git /XF @excluded | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Source-free validation copy failed: $Source" }
}
# This is an export integration fixture, not a released or rebuilt Editor.
if (!$SkipAssembly) {
foreach ($part in @('Engine/Content','Engine/Managed','Tools/Agent','Tools/Scripting','Tools/Build',
    'Tools/Content','Tools/Dependencies','Tools/Packaging','Tools/Distribution','Tools/Android',
    'Engine/Source/Runtime/Platform/Web','Engine/Source/Runtime/Platform/WeChat')) {
    Copy-TestTree "$repo/$part" "$root/$part" -NoNative:($part.StartsWith('Engine/Source/'))
}
Copy-TestTree $payload.Root "$root/ExportTemplates/$platform"
$null = New-Item -ItemType Directory -Path "$root/CMake"
foreach ($name in @('PrepareCookInput.cmake','VerifyGamePackage.cmake')) { Copy-Item "$repo/CMake/$name" "$root/CMake" }
$runtime = & "$repo/Tools/Scripting/GetPinnedRuntime.ps1"
Copy-TestTree $runtime.Root "$root/Scripting/CSharp/Runtime"
foreach ($name in @('cmake','node')) {
    $localTool = "$repo/out/agent/dependencies/windows/$name"
    if (Test-Path -LiteralPath $localTool) { Copy-TestTree $localTool "$root/out/agent/dependencies/windows/$name" }
}
Write-HEWorkspace "$root/HEngine.distribution.json" ([ordered]@{
    SchemaVersion=1;EngineVersion=(Get-HEEngineVersion $repo);Platform=$platform;Purpose='ExportIntegrationFixture'
})
}
$fixture = Get-Content -Raw -LiteralPath "$root/HEngine.distribution.json" | ConvertFrom-Json
if ($fixture.Purpose -ne 'ExportIntegrationFixture') {
    if (!$SkipAssembly -or $fixture.Editor -ne 'HEngineEditor.exe') { throw 'Use an export fixture or a complete binary Editor installation.' }
    & "$PSScriptRoot/Test-Distribution.ps1" -Engine $root
}
$null = Get-HEExportTemplate $root $platform
foreach ($forbidden in @('CMakeLists.txt','Engine/Source/Runtime/Launch','Engine/Source/Game','Project')) {
    if (Test-Path -LiteralPath "$root/$forbidden") { throw "Source-free fixture contains $forbidden" }
}
if (@(Get-ChildItem -LiteralPath "$root/Engine/Source" -Recurse -File | Where-Object {
    $_.Extension -in @('.cpp','.h') -or $_.Name -eq 'CMakeLists.txt'
}).Count) {
    throw 'Export fixture contains native engine source.'
}
$oldPath = $env:PATH
$oldEngine = $env:HE_ENGINE_ROOT
$logRoot = "$root/out/agent/windows-validation/$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $logRoot -Force
try {
    $env:PATH = ($oldPath.Split(';') | Where-Object { $_ -notmatch '(?i)Visual Studio|Microsoft Visual Studio|VC[\\/]Tools' }) -join ';'
    $env:HE_ENGINE_ROOT = $root
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/Tools/Build/Publish.ps1" `
            -Project $projectInfo.Root -Profile windows -Action Build *> "$logRoot/export.log"
        $exportCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($exportCode -ne 0) { throw "Source-free export failed; see $logRoot/export.log and the project's Saved/Build/windows.json." }
    $receipt = Get-Content -Raw -Encoding UTF8 -LiteralPath "$($projectInfo.Root)/Saved/Build/windows.json" | ConvertFrom-Json
    if (!$receipt.success) { throw 'Export has no success receipt.' }
    $actual = Get-FileHash -LiteralPath "$($receipt.output)/$($projectInfo.ExecutableName).exe"
    $expected = Get-FileHash -LiteralPath $payload.Files[$payload.Manifest.Player]
    if ($actual.Hash -cne $expected.Hash) { throw 'Exporter changed or rebuilt the native player.' }
    $manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath "$($receipt.output)/Game.manifest.json" | ConvertFrom-Json
    if ($manifest.Player.ProjectId -cne $projectInfo.ProjectId) { throw 'Export retained the template project identity.' }
    [ordered]@{schema=1;evidence='test-layer';pass=$true;engine=$root;project=$projectInfo.Root;
        output=$receipt.output;playerSha256=$actual.Hash.ToLowerInvariant();nativeSourcePresent=$false;
        validation='Shipping dependency/policy and WARP full-start gates executed by exporter'} |
        ConvertTo-Json | Set-Content -LiteralPath "$logRoot/result.json" -Encoding UTF8
    Write-Output "Source-free Windows export PASS: $($receipt.output); evidence=$logRoot/result.json"
} finally { $env:PATH=$oldPath; $env:HE_ENGINE_ROOT=$oldEngine }
