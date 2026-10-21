$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OutputDirectory.ps1')
$testRoot = Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../out/agent'))) ('publish-path-test-' + [Guid]::NewGuid().ToString('N'))
$project = Join-Path $testRoot '项目 with spaces'
$stage = Join-Path $testRoot 'stage'
$output = Join-Path $testRoot 'external output'
New-Item -ItemType Directory -Path $project, $stage, $output -Force | Out-Null
function Assert-Rejected([scriptblock]$Action) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw 'Expected unsafe or foreign output to be rejected.' }
}
if ((Resolve-HEBuildOutput $project 'Dist/Windows') -ne (Join-Path $project 'Dist/Windows')) { throw 'Relative resolution failed.' }
if ((Resolve-HEBuildOutput $project $output) -ne $output) { throw 'Absolute resolution failed.' }
Assert-Rejected { Resolve-HEBuildOutput $project '' }
Assert-Rejected { Resolve-HEBuildOutput $project $project }
Assert-Rejected { Resolve-HEBuildOutput $project $testRoot }
Assert-Rejected { Resolve-HEBuildOutput $project 'C:\' }
Assert-Rejected { Resolve-HEBuildOutput $project 'C:relative' }
Set-Content -LiteralPath (Join-Path $output 'personal.txt') -Value 'keep'
Assert-Rejected { Assert-HEBuildOutputOwner $output $project 'windows' }
Assert-Rejected { Assert-HEBuildOutputOwner $output $project 'windows' -RequireBuild }
$emptyOutput = Join-Path $testRoot 'empty output'
New-Item -ItemType Directory -Path $emptyOutput | Out-Null
@{ project = $project; profile = 'windows'; schema = 1 } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage '.he-build.json') -Encoding UTF8
Set-Content -LiteralPath (Join-Path $stage 'game.txt') -Value 'first'
Publish-HEBuildOutput $stage $emptyOutput $project 'windows'
Assert-HEBuildOutputOwner $emptyOutput $project 'windows' -RequireBuild
Assert-Rejected { Assert-HEBuildOutputOwner $emptyOutput $project 'other' }
Set-Content -LiteralPath (Join-Path $stage 'game.txt') -Value 'second'
Publish-HEBuildOutput $stage $emptyOutput $project 'windows'
if ((Get-Content -LiteralPath (Join-Path $emptyOutput 'game.txt')) -ne 'second') { throw 'Replacement failed.' }
if ((Get-Content -LiteralPath (Join-Path $output 'personal.txt')) -ne 'keep') { throw 'Foreign output changed.' }
$previous = @(Get-ChildItem -LiteralPath $testRoot -Directory -Filter 'empty output.previous-*' | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'game.txt') })
if ($previous.Count -ne 1 -or (Get-Content -LiteralPath (Join-Path $previous[0].FullName 'game.txt')) -ne 'first') { throw 'Previous build was not preserved.' }
Assert-Rejected { Publish-HEBuildOutput (Join-Path $testRoot 'missing-stage') $emptyOutput $project 'windows' }
if ((Get-Content -LiteralPath (Join-Path $emptyOutput 'game.txt')) -ne 'second') { throw 'Failed staging changed output.' }
Write-Output "unit-test PASS: relative/absolute paths, empty directories, ownership, replacement, preserved output and staging failure; evidence: $testRoot"
