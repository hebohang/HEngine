$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$PSScriptRoot/Template.ps1"
$work = Join-Path $repo ('out/agent/distribution-contract/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path "$work/Player" -Force
$player = Join-Path $work 'Player/Game.exe'
[IO.File]::WriteAllText($player, 'fixture, not an executable')
$runtimeContract = "$work/Player/HEngine.Runtime.json"
[IO.File]::WriteAllText($runtimeContract, '{"schema":1,"fsr":false,"dx12":false,"vulkan":true,"oidn":false}')
$manifest = [ordered]@{
    SchemaVersion=1;EngineVersion='1.1.0';Platform='Windows-x64';RuntimeFingerprint=('a'*64)
    Configuration='Release';PackGame=$true;DevAgent=$false
    ScriptApiHash=(Get-FileHash -LiteralPath "$repo/Tools/Scripting/Api.schema.json").Hash.ToLowerInvariant()
    Player='Player/Game.exe';Cooker='Player/Game.exe';ShaderCompiler='Player/Game.exe'
    Files=@(@{Path='Player/Game.exe';Sha256=(Get-FileHash -LiteralPath $player).Hash.ToLowerInvariant()},
        @{Path='Player/HEngine.Runtime.json';Sha256=(Get-FileHash -LiteralPath $runtimeContract).Hash.ToLowerInvariant()})
}
function Save-Template { $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$work/Template.json" -Encoding UTF8 }
function Assert-Rejected([scriptblock]$Operation) {
    $rejected=$false
    try { $null = & $Operation } catch { $rejected=$true }
    if (!$rejected) { throw 'Invalid template was accepted.' }
}
Save-Template
$null = Get-HEExportTemplate $repo 'Windows-x64' $work
$currentApi = $manifest.ScriptApiHash
$manifest.ScriptApiHash = '0' * 64; Save-Template
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work }
$manifest.ScriptApiHash = $currentApi; Save-Template
Assert-Rejected { Get-HEExportTemplate $repo 'Android-arm64' $work }
Assert-Rejected { Resolve-HEExportTemplateFile $work '../Player/Game.exe' }
Assert-Rejected { Resolve-HEExportTemplateFile $work '/Player/Game.exe' }
$manifest.Files += @{Path='player/game.exe';Sha256=$manifest.Files[0].Sha256}
Save-Template
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work }
$manifest.Files = @($manifest.Files[0], $manifest.Files[1]); $manifest.DevAgent=$true
Save-Template
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work }
$manifest.PackGame=$false; Save-Template
$null = Get-HEExportTemplate $repo 'Windows-x64' $work -Development
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work }
$manifest.DevAgent=$false; $manifest.PackGame=$true; Save-Template
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work -Development }
[IO.File]::AppendAllText($player, 'modified')
Assert-Rejected { Get-HEExportTemplate $repo 'Windows-x64' $work }
Write-Output 'unit-test PASS: template version/platform/distribution and payload integrity; path/duplicate rejection (fixture only).'
