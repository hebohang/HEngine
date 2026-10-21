param([string]$EvidenceDirectory = 'out/agent/android-script-dependencies')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$work = [IO.Path]::GetFullPath((Join-Path $repo $EvidenceDirectory))
New-Item -ItemType Directory -Force (Join-Path $work 'Assets') | Out-Null
function Write-Json($path, $value) { $value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $path -Encoding UTF8 }
Write-Json (Join-Path $work 'Assets/Main.he') @{entity=@{ScriptComponent=@{Backend='he.script.csharp';Type='test.android';Enabled=$true}}}
$metadata = Join-Path $work 'metadata.json'
$manifest = Join-Path $work 'manifest.json'
Write-Json $metadata @{types=@(@{id='test.android';dependencies=@('Assets/MobilePause.hui')},@{id='unused.sample';dependencies=@('Assets/Unused.png')})}
$verify = Join-Path $PSScriptRoot '../VerifyScriptDependencies.ps1'
Write-Json $manifest @{Files=@(@{Path='Assets/Main.he'})}
$rejected = $false
try { & $verify -Project $work -Metadata $metadata -Manifest $manifest }
catch {
    if ($_.Exception.Message -notlike "*requires uncooked resource 'Assets/MobilePause.hui'*") { throw }
    $rejected = $true
}
if (!$rejected) { throw 'Missing Android-only dependency was accepted.' }
Write-Json $manifest @{Files=@(@{Path='Assets/Main.he'},@{Path='Assets/MobilePause.hui'})}
& $verify -Project $work -Metadata $metadata -Manifest $manifest
Write-Output 'unit-test PASS: rejects missing mobile UI; accepts complete content without unused sample assets.'
