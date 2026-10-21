param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Profile = 'windows',
    [ValidateSet('Windows','Android')][string]$Platform = 'Windows',
    [ValidateSet('','Desktop','Portable')][string]$Quality = '',
    [ValidateSet('Check','Build')][string]$Action = 'Build',
    [string]$OutputDirectory = '',
    [string]$WorkDirectory = '',
    [string]$Template = ''
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
. "$repo/Tools/Dependencies/Environment.ps1"
. "$PSScriptRoot/Template.ps1"
Import-HEDependencyEnvironment
$info = Resolve-HEProject $Project $repo
$projectRoot = $info.Root
$descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $info.DescriptorPath | ConvertFrom-Json
if ($descriptor.RuntimeMode -ne 'Scene') { throw 'Precompiled export requires RuntimeMode=Scene. Native project code requires a custom player or the source workflow.' }
$profiles = Get-Content -Raw -Encoding UTF8 -LiteralPath "$projectRoot/Config/BuildProfiles.json" | ConvertFrom-Json
$selected = @($profiles.profiles | Where-Object id -eq $Profile)
if ($profiles.schema -ne 1 -or $selected.Count -ne 1 -or $selected[0].platform -ne $Platform -or $selected[0].configuration -ne 'Release') {
    throw "Select one Release $Platform profile."
}
$selected = $selected[0]
$templatePlatform = if ($Platform -eq 'Android') { 'Android-arm64' } else { Get-HEWindowsTemplatePlatform $repo }
$templateData = Get-HEExportTemplate $repo $templatePlatform $Template
$contract = $templateData.Manifest
if ($Platform -eq 'Windows') {
foreach ($required in @('Policy/PackageTargetGraph.dot','Policy/HGame-Release.map')) {
    if (!$templateData.Files.ContainsKey($required)) { throw "Windows template is missing policy evidence: $required" }
}
}
if ($Platform -eq 'Android') {
    $androidConfig = if ($selected.androidConfig) { $selected.androidConfig } else { 'Config/Android.json' }
    $androidSettings = Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-HEExportTemplateFile $projectRoot $androidConfig) | ConvertFrom-Json
    foreach ($name in @('gameConfig','pluginConfig','managedProject')) {
        $selected | Add-Member -MemberType NoteProperty -Name $name -Value $androidSettings.$name -Force
    }
    if ($contract.MinimumSdk -gt $androidSettings.minimumSdk) { throw 'The Android template requires a newer minimumSdk.' }
}
if ($contract.ScriptApiHash -cne (Get-FileHash -LiteralPath "$repo/Tools/Scripting/Api.schema.json").Hash.ToLowerInvariant()) {
    throw 'The export template and script SDK have different APIs.'
}
$gameConfig = if ($selected.gameConfig) { $selected.gameConfig } else { $descriptor.GameConfig }
$null = Resolve-HEExportTemplateFile $projectRoot $gameConfig
$pluginConfig = if ($selected.pluginConfig) { $selected.pluginConfig } else { 'Config/Plugins.json' }
$plugins = Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-HEExportTemplateFile $projectRoot $pluginConfig) | ConvertFrom-Json
foreach ($plugin in $plugins.PSObject.Properties) {
    $game = $plugin.Value.PSObject.Properties['Game']
    if ($game -and (($game.Value -eq $true -and $plugin.Name -notin $contract.Plugins) -or
        ($game.Value -eq $false -and $plugin.Name -in $contract.Plugins))) {
        throw "Selected plugin $($plugin.Name) does not match this precompiled player. Choose a matching template or the source workflow."
    }
}
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$dotnet = (Get-Command dotnet.exe -ErrorAction Stop).Source
$python = $env:HE_BUILD_PYTHON
if (!$python) { $python = (Get-Command python.exe -ErrorAction Stop).Source }
function Run-Tool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Export tool failed: $Tool ($LASTEXITCODE)" }
}
Push-Location "$repo/Engine/Managed"
try {
    $sdk = (& $dotnet --version).Trim()
    $expectedSdk = (Get-Content -Raw -LiteralPath global.json | ConvertFrom-Json).sdk.version
    if ($LASTEXITCODE -ne 0 -or $sdk -ne $expectedSdk) { throw "Install the pinned .NET SDK $expectedSdk." }
} finally { Pop-Location }
Run-Tool $python @('-X','utf8',"$repo/Tools/Build/Preflight.py",'--project',$projectRoot,'--profile',$Profile,'--precompiled-player')
if ($Action -eq 'Check') { Write-Output "$Platform template CHECK PASS: no native compiler is required."; return }
if (!$OutputDirectory) { throw 'Supply a fresh staging OutputDirectory; Publish.ps1 owns final output replacement.' }
$stage = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $stage) -and @(Get-ChildItem -LiteralPath $stage -Force).Count) { throw 'Export staging directory must be empty.' }
if (!$WorkDirectory) { $WorkDirectory = Join-Path $repo "out/agent/sdk-export/$($info.CacheKey)/$([guid]::NewGuid().ToString('N'))" }
$work = [IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $work) { throw 'Export work directory must be fresh.' }
$null = New-Item -ItemType Directory -Path $stage,$work -Force
$generated = Join-Path $work 'generated'
function Extended-Path([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute.StartsWith('\\')) { return '\\?\UNC\' + $absolute.Substring(2) }
    return '\\?\' + $absolute
}
foreach ($relative in $templateData.Files.Keys) {
    $target = $null
    if ($relative.StartsWith('Player/')) {
        $outputName = $relative.Substring(7)
        if ($Platform -eq 'Windows' -and $relative -ceq $contract.Player) { $outputName = $info.ExecutableName + '.exe' }
        $target = Join-Path $stage $outputName
    } elseif ($relative.StartsWith('GeneratedContent/Engine/')) {
        $target = Join-Path $generated $relative.Substring(24)
    }
    if ($target) {
        $null = [IO.Directory]::CreateDirectory((Extended-Path (Split-Path $target)))
        [IO.File]::Copy((Extended-Path $templateData.Files[$relative]),(Extended-Path $target),$false)
    }
}
$quality = if ($Quality) { $Quality } elseif ($selected.quality) { $selected.quality } else { 'Desktop' }
$managedProject = if ($selected.managedProject) { $selected.managedProject } else { 'Scripts/CSharp/GameScripts.csproj' }
$null = Resolve-HEExportTemplateFile $projectRoot $managedProject
& "$repo/Tools/Scripting/BuildManaged.ps1" -Project $projectRoot -WorkDirectory "$work/managed" `
    -OutputDirectory "$work/scripts" -Dotnet $dotnet -CMake $cmake -Python $python -Shipping -Quality $quality -IsolatedCompiler `
    -Platform $Platform -ManagedProject $managedProject
if ($LASTEXITCODE -ne 0) { throw 'Game script compilation failed.' }
$shaderCompiler = $templateData.Files[$contract.ShaderCompiler]
if (Test-Path -LiteralPath "$projectRoot/Shaders") {
    Push-Location (Split-Path $shaderCompiler)
    try {
        foreach ($shader in Get-ChildItem -LiteralPath "$projectRoot/Shaders" -Recurse -Filter '*.hlsl' -File) {
            $relativeShader = $shader.FullName.Substring($projectRoot.Length+1).Replace('\','/')
            if ($relativeShader -match '(^|/)DevOnly/') { continue }
            $null = Resolve-HEExportTemplateFile $projectRoot $relativeShader
            Run-Tool $shaderCompiler @('--shader',$shader.FullName,'--rootPath',"$projectRoot/Shaders",'--outputPath',"$generated/Shaders",'--includePath',"$repo/Engine/Content/Shaders",'--binary-format','all')
        }
    } finally { Pop-Location }
}
$runId = [guid]::NewGuid().ToString('N')
# WorkDirectory is already unique and must be fresh. Repeating its run UUID
# here pushes ordinary relocated SDK paths beyond native Windows path limits.
$cookInput = Join-Path $work 'CookInput-0'
Run-Tool $cmake @("-DENGINE_CONTENT_DIR=$repo/Engine/Content","-DENGINE_GENERATED_CONTENT_DIR=$generated","-DPROJECT_DIR=$projectRoot",
    "-DCOOK_INPUT_DIR=$cookInput","-DCOOK_INPUT_PARENT=$work","-DPROJECT_ID=$($info.ProjectId)",'-DPYTHON_PLUGIN_ENABLED=OFF','-P',"$repo/CMake/PrepareCookInput.cmake")
$cooker = $templateData.Files[$contract.Cooker]
Run-Tool $cooker @('--project',$cookInput,'--config',$gameConfig,'--engine-config',"$repo/Engine/Content/Config/EngineCook.json",
    '--python-plugin','disabled','--csharp-plugin','enabled','--csharp-metadata',"$work/scripts/GameScripts.metadata.json",
    '--forbid-root',$projectRoot,'--forbid-root',"$repo/Engine/Content",'--forbid-root',$generated,
    '--output',$stage,'--report',"$work/CookReport.json",'--run-id',$runId)
& "$repo/Tools/Packaging/Set-PlayerManifest.ps1" -Project $projectRoot -GameConfig $gameConfig `
    -TemplateContract "$($templateData.Root)/Template.json" -Manifest "$stage/Game.manifest.json"
if ($Platform -eq 'Windows') {
    & "$repo/Tools/Scripting/StageRuntime.ps1" -SourceDirectory "$work/scripts" -DestinationDirectory "$stage/Scripting/CSharp"
} else {
    & "$repo/Tools/Android/VerifyScriptDependencies.ps1" -Project $projectRoot -Metadata "$work/scripts/GameScripts.metadata.json" -Manifest "$stage/Game.manifest.json"
}
$packageConfig = Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-HEExportTemplateFile $projectRoot $descriptor.PackageConfig) | ConvertFrom-Json
Copy-Item -LiteralPath (Resolve-HEExportTemplateFile $projectRoot $packageConfig.ThirdPartyNotices) -Destination "$stage/THIRD_PARTY_NOTICES.txt" -Force
$licenseNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in Get-ChildItem -LiteralPath "$stage/Licenses" -File) { $null = $licenseNames.Add($file.Name) }
foreach ($license in $packageConfig.Licenses) {
    if ($license.Output -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*[.]txt$' -or $license.Output.Length -gt 128 -or
        $license.Output -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:[.]|$)' -or !$licenseNames.Add($license.Output)) {
        throw 'Project license output is unsafe or conflicts with an engine license.'
    }
    Copy-Item -LiteralPath (Resolve-HEExportTemplateFile $projectRoot $license.Source) -Destination "$stage/Licenses/$($license.Output)"
}
[IO.File]::WriteAllLines("$work/ExpectedPackageLicenses.txt", @($licenseNames | Sort-Object), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText("$stage/.he-package-root", "HEngine $($info.ProjectId) Package v1`n", [Text.UTF8Encoding]::new($false))
if ($Platform -eq 'Android') {
    Write-Output "Android template content PASS: $stage"
    return
}
$readme = @"
$($info.DisplayName) - Windows x64

Run $($info.ExecutableName).exe. Keep all files and subdirectories together.
The package includes its C# runtime; no engine checkout or compiler is required.
DX12 is the default; --graphics-api=vulkan selects Vulkan 1.3 when supported by
the installed graphics driver. The package does not install graphics drivers.

Startup log: %LOCALAPPDATA%\HEngine\Projects\$($info.ProjectId)\Saved\Logs\HEngine.log
"@
[IO.File]::WriteAllText("$stage/README.txt", $readme, [Text.UTF8Encoding]::new($false))
Run-Tool $cmake @("-DPACKAGE_DIR=$stage","-DPACKAGE_EXECUTABLE_NAME=$($info.ExecutableName)","-DPACKAGE_PROJECT_ID=$($info.ProjectId)",
    "-DPACKAGE_LICENSE_LIST_FILE=$work/ExpectedPackageLicenses.txt",'-DPACKAGE_PYTHON_PLUGIN=disabled','-DPACKAGE_CSHARP_PLUGIN=enabled','-P',"$repo/CMake/VerifyGamePackage.cmake")
& "$repo/Tools/Packaging/Verify-WindowsPackage.ps1" -PackageDir $stage -ExecutableName $info.ExecutableName `
    -Architecture ($templatePlatform.Substring('Windows-'.Length)) `
    -ExpectedD3D12SDKVersion $contract.D3D12SDKVersion -ExpectedAgilityPackageVersion $contract.AgilityPackageVersion `
    -TargetGraphPath $templateData.Files['Policy/PackageTargetGraph.dot'] -LinkMapPath $templateData.Files['Policy/HGame-Release.map'] `
    -RequireTargetGraphAndLinkMap -PythonPlugin disabled -CSharpPlugin enabled
& "$repo/Tools/Packaging/Test-PackagedGame.ps1" -PackageDir $stage -ExecutableName $info.ExecutableName -LogDirectory "$work/validation" -UseWarp
Write-Output "Windows template export PASS: $stage"
