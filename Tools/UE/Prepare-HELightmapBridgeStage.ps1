param(
    [Parameter(Mandatory = $true)] [string] $SourceProject,
    [Parameter(Mandatory = $true)] [string] $StageDir
)

$ErrorActionPreference = 'Stop'
$source = Get-Item -LiteralPath $SourceProject
if ($source.Extension -ne '.uproject') { throw 'SourceProject must be a .uproject file' }
$sourceRoot = [IO.Path]::GetFullPath($source.DirectoryName)
$stage = [IO.Path]::GetFullPath($StageDir)
$plugin = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'HELightmapBridge'))
if (!(Test-Path -LiteralPath (Join-Path $sourceRoot 'Content'))) { throw 'UE Content directory is missing' }
if (!(Test-Path -LiteralPath (Join-Path $plugin 'HELightmapBridge.uplugin'))) { throw 'HELightmapBridge plugin is missing' }
if ($stage.StartsWith($sourceRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase) -or
    $sourceRoot.StartsWith($stage + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage and source project must be separate directories'
}
if (Test-Path -LiteralPath $stage) { throw 'StageDir already exists; reuse or choose a fresh directory' }

New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -LiteralPath $source.FullName -Destination $stage
$sourceConfig = Join-Path $sourceRoot 'Config'
if (Test-Path -LiteralPath $sourceConfig) {
    Copy-Item -LiteralPath $sourceConfig -Destination $stage -Recurse
}
New-Item -ItemType Junction -Path (Join-Path $stage 'Content') -Target (Join-Path $sourceRoot 'Content') | Out-Null
$stagePlugins = Join-Path $stage 'Plugins'
New-Item -ItemType Directory -Path $stagePlugins | Out-Null
$sourcePlugins = Join-Path $sourceRoot 'Plugins'
if (Test-Path -LiteralPath $sourcePlugins) {
    foreach ($sourcePlugin in Get-ChildItem -LiteralPath $sourcePlugins -Directory) {
        if ($sourcePlugin.Name -ieq 'HELightmapBridge') { throw 'Source project already has HELightmapBridge' }
        New-Item -ItemType Junction -Path (Join-Path $stagePlugins $sourcePlugin.Name) -Target $sourcePlugin.FullName | Out-Null
    }
}
New-Item -ItemType Junction -Path (Join-Path $stagePlugins 'HELightmapBridge') -Target $plugin | Out-Null
Write-Output (Join-Path $stage $source.Name)
