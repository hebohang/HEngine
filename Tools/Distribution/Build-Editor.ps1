param([string]$Output = '', [switch]$SkipBuild, [string]$WindowsTemplate = '', [string]$AndroidTemplate = '', [string]$DevelopmentTemplate = '', [ValidateSet('', 'x64', 'arm64')][string]$Architecture = '', [string]$EditorBuildDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
. "$PSScriptRoot/Template.ps1"
. "$repo/Tools/Agent/Architecture.ps1"
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$platform = "Windows-$Architecture"
$version = Get-HEEngineVersion $repo
if (!$Output) { $Output = Join-Path $repo "dist/HEngine-$version-$platform" }
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw "Destination already exists: $destination. Choose a fresh release directory." }
if (!$AndroidTemplate) { $AndroidTemplate = Join-Path $repo 'out/agent/export-templates/Android-arm64' }
$null = Get-HEExportTemplate $repo 'Android-arm64' $AndroidTemplate
if (!$DevelopmentTemplate) { $DevelopmentTemplate = Join-Path $repo "out/agent/development-templates/$platform" }
$null = Get-HEExportTemplate $repo $platform $DevelopmentTemplate -Development
if (!$WindowsTemplate) {
    $templateName = if ($SkipBuild) { $platform } else { "$platform-" + [guid]::NewGuid().ToString('N') }
    $WindowsTemplate = Join-Path $repo "out/agent/export-templates/$templateName"
}
if (!(Test-Path -LiteralPath $WindowsTemplate) -and !$SkipBuild) {
    & "$PSScriptRoot/Build-WindowsTemplate.ps1" -Output $WindowsTemplate -Architecture $Architecture
}
$null = Get-HEExportTemplate $repo $platform $WindowsTemplate
$project = Resolve-HEProject "$repo/Engine/Templates/Blank" $repo
$mode = if ($Architecture -eq 'x64') { 'editor-portable' } else { "editor-portable-$Architecture" }
$build = Join-Path $repo "out/agent/projects/$($project.CacheKey)/$mode-relwithdebinfo"
if ($EditorBuildDirectory) {
    if (!$SkipBuild) { throw 'EditorBuildDirectory requires SkipBuild and an already built Editor payload.' }
    $build = [IO.Path]::GetFullPath($EditorBuildDirectory)
}
if (!$SkipBuild) {
    & "$repo/Tools/Agent/Build.ps1" -Target HEngineEditor -Configuration RelWithDebInfo -Project $project.Root -PortableEditor -PluginConfig "$PSScriptRoot/Plugins.json" -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw 'Portable Editor build failed.' }
}
if (!(Test-Path -LiteralPath "$build/bin/HEngineEditor.exe")) { throw 'Portable Editor executable is missing.' }
$stage = $destination + '.staging-' + [guid]::NewGuid().ToString('N')
$null = New-Item -ItemType Directory -Path $stage
function Copy-Tree([string]$source, [string]$target, [switch]$PreserveBin, [switch]$NoNative) {
    $null = New-Item -ItemType Directory -Force -Path $target
    $excluded = @('obj','__pycache__','.git','HEngineFsrSmoke','HEngineDX12Smoke',
        'HEngineDX12SceneSmoke','HEffekseerDX12InteropSmoke','HEngineImGuiDX12Smoke','HEngineRmlDX12Smoke')
    if (!$PreserveBin) { $excluded += 'bin' }
    $excludedFiles = @('*.pdb','*.ilk','*.pyc')
    if ($NoNative) { $excludedFiles += @('*.cpp','*.h','*.hpp','*.c','CMakeLists.txt') }
    & robocopy.exe $source $target /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD @excluded /XF @excludedFiles | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed: $source" }
}
Copy-Tree "$build/bin" $stage
Copy-Tree "$repo/Engine/Content" "$stage/Engine/Content"
Copy-Tree "$build/GeneratedContent/Engine" "$stage/Engine/Content"
Copy-Tree "$repo/Engine/Resources" "$stage/Engine/Resources"
Copy-Tree "$repo/Engine/Templates" "$stage/Engine/Templates"
Copy-Tree "$repo/Engine/Managed" "$stage/Engine/Managed"
Copy-Tree $WindowsTemplate "$stage/ExportTemplates/$platform" -PreserveBin
Copy-Tree $AndroidTemplate "$stage/ExportTemplates/Android-arm64" -PreserveBin
Copy-Tree $DevelopmentTemplate "$stage/DevelopmentTemplates/$platform" -PreserveBin
Copy-Tree "$WindowsTemplate/Player/Licenses" "$stage/Licenses"
$editorLicenses = Get-Content -LiteralPath "$PSScriptRoot/EditorLicenses.json" -Raw | ConvertFrom-Json
$armLicenses = @{}
if ($Architecture -eq 'arm64') {
    $armRoot = if ($env:HE_WINDOWS_ARM64_ROOT) { $env:HE_WINDOWS_ARM64_ROOT } else { "$repo/out/agent/dependencies/windows-arm64" }
    $armPaths = Get-Content -LiteralPath "$armRoot/prepared.json" -Raw | ConvertFrom-Json
    $armLicenses = @{
        'Embree.txt' = "$($armPaths.embree)/LICENSE.txt"
        'Embree-third-party.txt' = "$($armPaths.embree)/third-party-programs.txt"
        'Embree-TBB.txt' = "$($armPaths.embree)/third-party-programs-TBB.txt"
        'DXC-MS.txt' = "$($armPaths.dxc)/LICENSE-MS.txt"
        'DXC-LLVM.txt' = "$($armPaths.dxc)/LICENSE-LLVM.txt"
        'Python-3.11.txt' = "$($armPaths.python)/tools/LICENSE.txt"
    }
    Copy-Item -LiteralPath "$repo/Tools/Dependencies/windows-arm64.lock.json" -Destination "$stage/Licenses/Windows-arm64-dependencies.json"
}
foreach ($entry in $editorLicenses.files.PSObject.Properties) {
    $licenseSource = if ($armLicenses.ContainsKey($entry.Name)) { $armLicenses[$entry.Name] } else { Join-Path $repo $entry.Value }
    Copy-Item -LiteralPath $licenseSource -Destination "$stage/Licenses/$($entry.Name)" -ErrorAction Stop
}
$licenseInventory = @(Get-ChildItem -LiteralPath "$stage/Licenses" -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{Path=$_.FullName.Substring($stage.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
Write-HEWorkspace "$stage/Licenses/EditorDependencies.json" ([ordered]@{SchemaVersion=1;Files=$licenseInventory})
[IO.File]::WriteAllText("$stage/THIRD_PARTY_NOTICES.txt", "HEngine third-party license texts are in Licenses/; EditorDependencies.json records their file hashes. Managed runtimes also retain their own LICENSE and THIRD-PARTY-NOTICES files. FSR uses FidelityFX SDK 1.1.4 / FSR 3.1.4. Optional offline CPU denoising uses OIDN 2.5.1 and its shared oneTBB runtime.", [Text.UTF8Encoding]::new($false))
Copy-Tree "$repo/Engine/Source/Runtime/Platform/Android/Managed" "$stage/Engine/Source/Runtime/Platform/Android/Managed"
foreach ($portablePlatform in @('Web','WeChat')) {
    Copy-Tree "$repo/Engine/Source/Runtime/Platform/$portablePlatform" "$stage/Engine/Source/Runtime/Platform/$portablePlatform" -NoNative
}
foreach ($directory in @('Agent','Scripting','Build','Distribution','Dependencies','Content','Packaging','Android')) {
    Copy-Tree "$repo/Tools/$directory" "$stage/Tools/$directory" -PreserveBin
}
$null = New-Item -ItemType Directory -Path "$stage/CMake" -Force
foreach ($script in @('PrepareCookInput.cmake','VerifyGamePackage.cmake')) {
    Copy-Item -LiteralPath "$repo/CMake/$script" -Destination "$stage/CMake"
}
Copy-Item -LiteralPath "$repo/hectl.bat" -Destination $stage
Copy-Item -LiteralPath "$PSScriptRoot/AgentInstructions.md" -Destination "$stage/AGENTS.md"
Copy-Item -LiteralPath "$repo/Win-ProjectManager.bat" -Destination $stage
Copy-Item -LiteralPath "$repo/LICENSE" -Destination $stage
Copy-Item -LiteralPath "$repo/RELEASE_NOTES.md" -Destination $stage
$null = New-Item -ItemType Directory -Force -Path "$stage/Docs"
foreach ($document in @('Release-1.1.0.md', 'Projects.md', 'StarterContent.md', 'WindowsGraphicsCompatibility.md', 'BuildAndPublish.md', 'ReleaseSDK-1.1.0.md', 'Scripting.md', 'SceneEditing.md', 'InputActions.md', 'ScriptingGuide.md', 'ScriptingApiAudit-1.1.0.md', 'ScriptingSdkValidation-1.1.0.md', 'AI_GAME_DEVELOPMENT.md')) {
    Copy-Item -LiteralPath "$repo/Docs/$document" -Destination "$stage/Docs/$document"
}
Copy-Tree "$repo/Examples/ScriptingBasics" "$stage/Examples/ScriptingBasics"
Copy-Item -LiteralPath "$repo/Tools/Distribution/llms.txt" -Destination "$stage/llms.txt"
& dotnet run --project "$repo/Tools/Scripting/ReferenceGenerator/ReferenceGenerator.csproj" --configuration Release -- `
    --assembly "$stage/Scripting/CSharp/HEngine.ScriptCore.dll" --xml "$stage/Scripting/CSharp/HEngine.ScriptCore.xml" --output "$stage/Docs/Api/csharp-signatures.json"
if ($LASTEXITCODE -ne 0) { throw 'Compiled C# API signature extraction failed.' }
& py -3.11 "$repo/Tools/Scripting/GenerateReference.py" --engine $stage
if ($LASTEXITCODE -ne 0) { throw 'Script SDK reference generation failed.' }
$runtime = & "$repo/Tools/Scripting/GetPinnedRuntime.ps1" -Architecture $Architecture
Copy-Tree $runtime.Root "$stage/Scripting/CSharp/Runtime"
# Python and the vendor DLLs depend on the licensed toolchain redistributable.
. "$repo/Tools/Agent/Toolchain.ps1"
Import-HeToolchain -Toolchain (Get-HeVisualStudio -Architecture $Architecture)
$crtCandidates = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir $Architecture) -Directory -Filter 'Microsoft.VC*.CRT' |
    ForEach-Object { Join-Path $_.FullName 'vcruntime140.dll' } | Where-Object { Test-Path -LiteralPath $_ })
if ($crtCandidates.Count -ne 1) { throw "The selected MSVC toolchain must provide exactly one $Architecture CRT runtime." }
$crtDirectory = Split-Path $crtCandidates[0]
foreach ($name in @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')) {
    if ($Architecture -eq 'arm64' -and $name -eq 'vcruntime140_1.dll' -and !(Test-Path -LiteralPath "$crtDirectory/$name")) { continue }
    Copy-Item -LiteralPath (Join-Path $crtDirectory $name) -Destination $stage -ErrorAction Stop
}
$managerExe = [IO.Path]::GetFullPath((Join-Path $stage 'ProjectManager.exe'))
& "$PSScriptRoot/Build-ProjectManager.ps1" -Output $managerExe -Architecture $Architecture
Write-HEWorkspace "$stage/HEngine.distribution.json" ([ordered]@{SchemaVersion=1;EngineVersion=$version;Platform=$platform;Editor='HEngineEditor.exe'})
& "$PSScriptRoot/Test-Distribution.ps1" -Engine $stage
& "$PSScriptRoot/Test-EditorDependencies.ps1" -Engine $stage
[IO.Directory]::Move($stage, $destination)
Write-Output "Editor distribution: $destination"

