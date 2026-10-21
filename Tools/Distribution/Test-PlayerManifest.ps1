$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$work = Join-Path $repo ('out/agent/distribution-contract/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $work -Force
$manifest = Join-Path $work 'Game.manifest.json'
$template = Join-Path $work 'Template.json'
$contract = [ordered]@{SchemaVersion=1;EngineVersion='1.1.0';Platform='Windows-x64';RuntimeFingerprint=('a' * 64);PackGame=$true;DevAgent=$false;Configuration='Release'}
$contract | ConvertTo-Json | Set-Content -LiteralPath $template -Encoding UTF8
'{"ManifestVersion":1,"Files":[{"Path":"Config/Game.json"}]}' | Set-Content -LiteralPath $manifest -Encoding UTF8
$arguments = @{Project=(Join-Path $repo 'Engine/Templates/Blank');TemplateContract=$template;Manifest=$manifest}
& "$repo/Tools/Packaging/Set-PlayerManifest.ps1" @arguments
$result = Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json
if ($result.Player.ProjectId -ne 'blank' -or $result.Player.RuntimeFingerprint -ne ('a' * 64)) { throw 'Player identity was not written.' }
$before = [IO.File]::ReadAllText($manifest)
function Assert-Rejected([scriptblock]$Operation) {
    $rejected = $false
    try { & $Operation } catch { $rejected = $true }
    if (!$rejected) { throw 'Invalid player manifest input was accepted.' }
    if ([IO.File]::ReadAllText($manifest) -cne $before) { throw 'Failure changed the previous package manifest.' }
}
Assert-Rejected { & "$repo/Tools/Packaging/Set-PlayerManifest.ps1" @arguments -GameConfig '../escape.json' }
Assert-Rejected { & "$repo/Tools/Packaging/Set-PlayerManifest.ps1" @arguments -GameConfig 'Config/Missing.json' }
$contract.PackGame = $false
$contract.DevAgent = $true
$contract | ConvertTo-Json | Set-Content -LiteralPath $template -Encoding UTF8
Assert-Rejected { & "$repo/Tools/Packaging/Set-PlayerManifest.ps1" @arguments }
Write-Output 'unit-test PASS: player package identity; path/missing config/dev template rejection; failures preserve the manifest.'
