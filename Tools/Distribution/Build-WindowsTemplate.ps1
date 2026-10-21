param([string]$Output = '', [switch]$SkipBuild, [string]$PackageOutputRoot = '', [ValidateSet('', 'x64', 'arm64')][string]$Architecture = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
. "$PSScriptRoot/Template.ps1"
. "$repo/Tools/Agent/Architecture.ps1"
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$platform = "Windows-$Architecture"
$project = Resolve-HEProject "$repo/Engine/Templates/Blank" $repo
if (!$Output) { $Output = Join-Path $repo "out/agent/export-templates/$platform" }
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw "Template output already exists: $destination" }
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $identity = "$($project.ProjectId)`n$($project.Root)`nportable-player".ToLowerInvariant()
    $key = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-','').ToLowerInvariant().Substring(0,12)
    if ($Architecture -ne 'x64') { $key = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes("$key`n$Architecture"))).Replace('-','').ToLowerInvariant().Substring(0,12) }
} finally { $hasher.Dispose() }
$build = Join-Path $repo "out/pkg/$key"
if (!$SkipBuild) {
    & "$repo/Tools/Agent/Package.ps1" -Project $project.Root -PluginConfig "$PSScriptRoot/PlayerPlugins.json" -PortablePlayer -OutputRoot $PackageOutputRoot -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw 'Portable player Shipping build failed.' }
}
$package = (Get-Content -Raw -LiteralPath "$build/GamePackage.directory").Trim()
$contract = Get-Content -Raw -LiteralPath "$build/PlayerTemplate-Release.json" | ConvertFrom-Json
$packageManifest = Get-Content -Raw -LiteralPath "$package/Game.manifest.json" | ConvertFrom-Json
if ($packageManifest.Player.RuntimeFingerprint -cne $contract.RuntimeFingerprint) { throw 'The verified package and template build are different.' }
# Even SkipBuild must validate the existing executable and dependencies before
# using them as the official export template.
$sdkVersion = [int](Get-Content -Raw -LiteralPath "$build/DirectXAgilitySDK.version").Trim()
$agilityVersion = (Get-Content -Raw -LiteralPath "$build/DirectXAgilitySDK.package-version").Trim()
& "$build/bin/Programs/HCooker/Release/HCooker.exe" --self-test
if ($LASTEXITCODE -ne 0) { throw 'Template cooker self-test failed.' }
if ($SkipBuild) {
& "$repo/Tools/Packaging/Verify-WindowsPackage.ps1" -PackageDir $package -ExecutableName $project.ExecutableName `
    -Architecture $Architecture `
    -ExpectedD3D12SDKVersion $sdkVersion -ExpectedAgilityPackageVersion $agilityVersion `
    -TargetGraphPath "$build/PackageTargetGraph.dot" -LinkMapPath "$build/PackagePolicy/HGame-Release.map" `
    -RequireTargetGraphAndLinkMap -PythonPlugin disabled -CSharpPlugin enabled
& "$repo/Tools/Packaging/Test-PackagedGame.ps1" -PackageDir $package -ExecutableName $project.ExecutableName -UseWarp
}
$stage = $destination + '.staging-' + [guid]::NewGuid().ToString('N')
$null = New-Item -ItemType Directory -Path "$stage/Player" -Force
function Copy-PayloadTree([string]$Source, [string]$Target) {
    $null = New-Item -ItemType Directory -Path $Target -Force
    & robocopy.exe $Source $Target /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XF *.pdb *.ilk *.lib *.exp | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Template copy failed: $Source" }
}
foreach ($item in Get-ChildItem -LiteralPath $package -Force) {
    if ($item.Name -in @('Game.pak','Game.manifest.json','.he-package-root','Scripting')) { continue }
    if ($item.PSIsContainer) { Copy-PayloadTree $item.FullName "$stage/Player/$($item.Name)" }
    else { Copy-Item -LiteralPath $item.FullName -Destination "$stage/Player" }
}
Copy-PayloadTree "$build/bin/Programs/HCooker/Release" "$stage/Tools/HCooker"
Copy-PayloadTree "$build/bin/Programs/HShaderCompiler" "$stage/Tools/HShaderCompiler"
Copy-PayloadTree "$build/GeneratedContent/Engine" "$stage/GeneratedContent/Engine"
$null = New-Item -ItemType Directory -Path "$stage/Policy"
Copy-Item -LiteralPath "$build/PackageTargetGraph.dot" -Destination "$stage/Policy/PackageTargetGraph.dot"
Copy-Item -LiteralPath "$build/PackagePolicy/HGame-Release.map" -Destination "$stage/Policy/HGame-Release.map"
Copy-Item -LiteralPath "$build/Engine/Source/Game/ExpectedPackageLicenses.txt" -Destination "$stage/Policy/ExpectedPackageLicenses.txt"
$values = @{
    Player="Player/$($project.ExecutableName).exe";Cooker='Tools/HCooker/HCooker.exe'
    ShaderCompiler='Tools/HShaderCompiler/HShaderCompiler.exe'
    D3D12SDKVersion=$sdkVersion;AgilityPackageVersion=$agilityVersion
}
foreach ($name in $values.Keys) { $contract | Add-Member -MemberType NoteProperty -Name $name -Value $values[$name] -Force }
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
    [ordered]@{Path=$_.FullName.Substring($stage.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
$contract | Add-Member -MemberType NoteProperty -Name Files -Value $files
[IO.File]::WriteAllText("$stage/Template.json", ($contract | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
$null = Get-HEExportTemplate $repo $platform $stage
$null = New-Item -ItemType Directory -Path (Split-Path $destination) -Force
[IO.Directory]::Move($stage, $destination)
Write-Output "Windows export template: $destination"
