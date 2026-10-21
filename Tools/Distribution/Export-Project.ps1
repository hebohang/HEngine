param(
    [string]$Project = '',
    [Parameter(Mandatory=$true)][string]$Output,
    [switch]$PrecompileCSharp
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
$info = Resolve-HEProject $Project $repo
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw "Destination exists: $destination" }
if ($destination.StartsWith($info.Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Export outside the source project to avoid recursively exporting previous output.'
}
$stage = $destination + '.staging-' + [guid]::NewGuid().ToString('N')
$null = New-Item -ItemType Directory -Path $stage
$files = Get-ChildItem -LiteralPath $info.Root -Recurse -File -Force | Where-Object {
    $relative = $_.FullName.Substring($info.Root.Length + 1)
    $relative -notmatch '(^|[\\/])(.git|Saved|Dist|SourceAssets|bin|obj|__pycache__|Prebuilt|\.he-content)([\\/]|$)' -and
    $relative -notmatch '(^|[\\/])HEngine.workspace.local.json' -and
    $relative -ne 'Config\EditorLayout.ini' -and $relative -ne 'Config/EditorLayout.ini' -and
    $_.Extension -notin @('.pyc','.pdb','.ilk','.log','.lock')
}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($info.Root.Length + 1)
    $target = Join-Path $stage $relative
    $null = New-Item -ItemType Directory -Force (Split-Path $target)
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
if ($PrecompileCSharp -and (Test-Path "$stage/Scripts/CSharp/GameScripts.csproj")) {
    $managedWork = "$repo/out/agent/release-project/$($info.CacheKey)/$([guid]::NewGuid().ToString('N'))"
    & "$repo/Tools/Scripting/BuildManaged.ps1" -Project $stage -WorkDirectory $managedWork `
        -OutputDirectory "$managedWork/output" -EditorHost -IsolatedCompiler
    if ($LASTEXITCODE -ne 0) { throw 'Independent project C# build failed.' }
    $null = New-Item -ItemType Directory -Force -Path "$stage/Scripts/CSharp/Prebuilt"
    Get-ChildItem -LiteralPath "$managedWork/output" -File | Where-Object Extension -NotIn @('.pdb','.ilk') |
        Copy-Item -Destination "$stage/Scripts/CSharp/Prebuilt"
}
$null = Resolve-HEProject $stage $repo
[IO.Directory]::Move($stage, $destination)
Write-Output "Independent project: $destination"
