param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [string]$BuildProfile = 'windows',
    [string]$Template = ''
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
. "$PSScriptRoot/Template.ps1"
$platform = Get-HEWindowsTemplatePlatform $repo
if (!$Template) { $Template = "$repo/DevelopmentTemplates/$platform" }
$data = Get-HEExportTemplate $repo $platform $Template -Development
$info = Resolve-HEProject $Project $repo
$descriptor = Get-Content -Raw -Encoding UTF8 -LiteralPath $info.DescriptorPath | ConvertFrom-Json
if ($descriptor.RuntimeMode -ne 'Scene') { throw 'The SDK development player requires RuntimeMode=Scene. Native project code requires the source workflow.' }
if ($data.Manifest.ScriptApiHash -cne (Get-FileHash -LiteralPath "$repo/Tools/Scripting/Api.schema.json").Hash.ToLowerInvariant()) {
    throw 'Development player and script SDK APIs do not match.'
}
$profiles = Get-Content -Raw -Encoding UTF8 -LiteralPath "$($info.Root)/Config/BuildProfiles.json" | ConvertFrom-Json
$selected = @($profiles.profiles | Where-Object id -eq $BuildProfile)
if ($profiles.schema -ne 1 -or $selected.Count -ne 1 -or $selected[0].platform -ne 'Windows') { throw 'Choose one Windows build profile for the development player.' }
$selected = $selected[0]
$gameConfig = if ($selected.gameConfig) { $selected.gameConfig } else { $descriptor.GameConfig }
$null = Resolve-HEExportTemplateFile $info.Root $gameConfig
$pluginConfig = if ($selected.pluginConfig) { $selected.pluginConfig } else { 'Config/Plugins.json' }
$plugins = Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-HEExportTemplateFile $info.Root $pluginConfig) | ConvertFrom-Json
foreach ($plugin in $plugins.PSObject.Properties) {
    $game = $plugin.Value.PSObject.Properties['Game']
    if ($game -and (($game.Value -eq $true -and $plugin.Name -notin $data.Manifest.Plugins) -or
        ($game.Value -eq $false -and $plugin.Name -in $data.Manifest.Plugins))) {
        throw "Plugin $($plugin.Name) does not match the SDK development player."
    }
}
$run = [IO.Path]::GetFullPath($RunDirectory)
$bin = Join-Path $run 'player/bin'
if (Test-Path -LiteralPath $bin) { throw 'Development player preparation needs a fresh session directory.' }
function Extended-Path([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute.StartsWith('\\')) { return '\\?\UNC\' + $absolute.Substring(2) }
    return '\\?\' + $absolute
}
foreach ($relative in $data.Files.Keys) {
    if (!$relative.StartsWith('Player/')) { continue }
    $name = $relative.Substring(7)
    if ($relative -ceq $data.Manifest.Player) { $name = $info.ExecutableName + '.exe' }
    $target = Join-Path $bin $name
    $null = [IO.Directory]::CreateDirectory((Extended-Path (Split-Path $target)))
    [IO.File]::Copy((Extended-Path $data.Files[$relative]),(Extended-Path $target),$false)
}
$managedProject = if ($selected.managedProject) { $selected.managedProject } else { 'Scripts/CSharp/GameScripts.csproj' }
$null = Resolve-HEExportTemplateFile $info.Root $managedProject
$quality = if ($selected.quality) { $selected.quality } else { 'Desktop' }
$buildLog = Join-Path $run 'scripts-build.log'
try {
    & "$repo/Tools/Scripting/BuildManaged.ps1" -Project $info.Root -WorkDirectory "$run/managed" `
        -OutputDirectory "$run/Saved/Scripting/CSharp" -ManagedProject $managedProject -Quality $quality `
        -Platform Windows -Configuration Release -IsolatedCompiler *> $buildLog
    if ($LASTEXITCODE -ne 0) { throw 'Managed compiler failed.' }
} catch { throw "Project script build failed. See $buildLog. $($_.Exception.Message)" }
$artifact = Join-Path $bin ($info.ExecutableName + '.exe')
$receipt = [ordered]@{schema=1;projectId=$info.ProjectId;profile=$BuildProfile;gameConfig=$gameConfig;
    nativeCompilation=$false;runtimeFingerprint=$data.Manifest.RuntimeFingerprint;
    nativeSha256=(Get-FileHash -LiteralPath $artifact).Hash.ToLowerInvariant()}
[IO.File]::WriteAllText((Join-Path $run 'development-player.json'),($receipt | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
return [pscustomobject]@{Artifact=$artifact;ProjectRoot=$info.Root;GameConfig=$gameConfig}
