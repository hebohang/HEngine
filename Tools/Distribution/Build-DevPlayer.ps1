param([string]$Output = '', [switch]$SkipBuild, [ValidateSet('', 'x64', 'arm64')][string]$Architecture = '',
      [ValidatePattern('^$|^[a-z0-9][a-z0-9-]{0,47}$')][string]$BuildVariant = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Project.ps1"
. "$PSScriptRoot/Template.ps1"
. "$repo/Tools/Agent/Architecture.ps1"
$Architecture = Resolve-HeWindowsArchitecture $Architecture
$platform = "Windows-$Architecture"
$project = Resolve-HEProject "$repo/Engine/Templates/Blank" $repo
if (!$Output) { $Output = "$repo/out/agent/development-templates/$platform" }
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw 'Choose a fresh development template output directory.' }
$cacheVariant = Get-HeArchitectureVariant $BuildVariant $Architecture
$mode = if ($cacheVariant) { "game-portable-$cacheVariant" } else { 'game-portable' }
$build = "$repo/out/agent/projects/$($project.CacheKey)/$mode-release"
if (!$SkipBuild) {
    & "$repo/Tools/Agent/Build.ps1" -Target HGame -Configuration Release -Project $project.Root -PortablePlayer -PluginConfig "$PSScriptRoot/PlayerPlugins.json" -Parallel 2 -BuildVariant $BuildVariant -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw 'Portable development player build failed.' }
}
$contract = Get-Content -Raw -LiteralPath "$build/PlayerTemplate-Release.json" | ConvertFrom-Json
if ($contract.PackGame -ne $false -or $contract.DevAgent -ne $true) { throw 'Expected an Agent-enabled development player.' }
$stage = $destination + '.staging-' + [guid]::NewGuid().ToString('N')
function Copy-DevTree([string]$Source, [string]$Target) {
    $null = New-Item -ItemType Directory -Path $Target -Force
    $excluded = @('obj','__pycache__','HEngineFsrSmoke','HEngineDX12Smoke',
        'HEngineDX12SceneSmoke','HEffekseerDX12InteropSmoke','HEngineImGuiDX12Smoke','HEngineRmlDX12Smoke')
    & robocopy.exe $Source $Target /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD @excluded /XF *.pdb *.ilk *.lib *.exp | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Development template copy failed: $Source" }
}
Copy-DevTree "$build/bin" "$stage/Player"
Copy-DevTree "$repo/Engine/Content" "$stage/Player/Engine/Content"
Copy-DevTree "$build/GeneratedContent/Engine" "$stage/Player/Engine/Content"
Copy-DevTree "$repo/Engine/Resources" "$stage/Player/Engine/Resources"
$runtime = & "$repo/Tools/Scripting/GetPinnedRuntime.ps1" -Architecture $Architecture
Copy-DevTree $runtime.Root "$stage/Player/Scripting/CSharp/Runtime"
$contract | Add-Member -MemberType NoteProperty -Name Player -Value "Player/$($project.ExecutableName).exe"
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{Path=$_.FullName.Substring($stage.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
$contract | Add-Member -MemberType NoteProperty -Name Files -Value $files
[IO.File]::WriteAllText("$stage/Template.json",($contract | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
$null = Get-HEExportTemplate $repo $platform $stage -Development
$null = New-Item -ItemType Directory -Path (Split-Path $destination) -Force
[IO.Directory]::Move($stage,$destination)
Write-Output "Development player template: $destination"
# Robocopy uses 1 for a successful copy. Report the script's actual outcome.
exit 0

