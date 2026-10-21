$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('he-storage-cli-' + [guid]::NewGuid().ToString('N'))
$fixtureRoot = [IO.Path]::GetFullPath($fixture)
$files = @('hectl.ps1','Storage.ps1','Build.ps1','Session.ps1','Package.ps1','CodeGen.ps1',
    'Profile-Tracy.ps1','Profile-TracyStartup.ps1','Benchmark-Rendering.ps1','Capture-PBRShadow.ps1',
    'Capture-RenderingBenchmark.ps1','Profile-RenderingWpr.ps1')
foreach ($file in $files) {
    $tokens = $null
    $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot "Tools/Agent/$file"), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) { throw "PowerShell parser failure in ${file}: $parseErrors" }
}

function Invoke-FixtureClean {
    param([string[]]$Arguments)
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $fixtureRoot 'Tools/Agent/hectl.ps1') clean @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Fixture CLI failed: $Arguments" }
    return (($output -join "`n") | ConvertFrom-Json)
}

function Get-FixtureSnapshot {
    return (@(Get-ChildItem -LiteralPath $fixtureRoot -Recurse -Force | Sort-Object FullName | ForEach-Object {
        if ($_.PSIsContainer) { "D:$($_.FullName)" }
        else { "F:$($_.FullName):$($_.LastWriteTimeUtc.Ticks):$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" }
    }) -join "`n")
}

try {
    $toolRoot = Join-Path $fixtureRoot 'Tools/Agent'
    $dependencyRoot = Join-Path $fixtureRoot 'Tools/Dependencies'
    $projectRoot = Join-Path $fixtureRoot 'Project'
    New-Item -ItemType Directory -Path $toolRoot,$dependencyRoot,$projectRoot | Out-Null
    foreach ($file in @('hectl.ps1','Toolchain.ps1','Project.ps1','Storage.ps1','storage.py')) {
        Copy-Item -LiteralPath (Join-Path $repositoryRoot "Tools/Agent/$file") -Destination $toolRoot
    }
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'Tools/Dependencies/Environment.ps1') -Destination $dependencyRoot
    'project(HEngine VERSION 1.1.0)' | Set-Content -LiteralPath (Join-Path $fixtureRoot 'CMakeLists.txt') -Encoding UTF8
    '{}' | Set-Content -LiteralPath (Join-Path $projectRoot 'GameConfig.json') -Encoding UTF8
    '{}' | Set-Content -LiteralPath (Join-Path $projectRoot 'PackageConfig.json') -Encoding UTF8
    @{SchemaVersion=1;ProjectId='fixture';DisplayName='Fixture';EngineVersion='1.1.0';ExecutableName='FixtureGame';GameConfig='GameConfig.json';PackageConfig='PackageConfig.json'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'Project.heproject') -Encoding UTF8
    $cacheRoot = Join-Path $fixtureRoot 'out/agent/editor-debug'
    New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
    "CMAKE_HOME_DIRECTORY:INTERNAL=$fixtureRoot" | Set-Content -LiteralPath (Join-Path $cacheRoot 'CMakeCache.txt') -Encoding UTF8
    New-Item -ItemType Directory -Path (Join-Path $cacheRoot 'Engine'),(Join-Path $cacheRoot 'sessions/pinned') -Force | Out-Null
    'object' | Set-Content -LiteralPath (Join-Path $cacheRoot 'Engine/object.obj') -Encoding UTF8
    '' | Set-Content -LiteralPath (Join-Path $cacheRoot 'sessions/pinned/.he-keep') -Encoding UTF8
    $before = Get-FixtureSnapshot
    $preview = Invoke-FixtureClean @('stale','-WhatIf','--json','-Report',(Join-Path $fixtureRoot 'must-not-exist.json'))
    if ($preview.deletedBytes -ne 0 -or $before -ne (Get-FixtureSnapshot)) { throw 'WhatIf changed the fixture' }
    $status = Invoke-FixtureClean @('status','--json')
    if (-not $status.scanComplete -or $status.reclaimableBytes -ne 0) { throw 'Unexpected empty fixture status' }
    $caches = Invoke-FixtureClean @('caches','--json')
    if (-not $caches.scanComplete -or $caches.caches.Count -ne 1 -or $caches.caches[0].state -ne 'cache') { throw 'Cache summary CLI failed' }
    $preview = Invoke-FixtureClean @('preview','--json','-KeepSuccess','1','-FailureDays','2','-BudgetBytes','1024')
    if ($preview.policy.successCount -ne 1 -or $preview.policy.failureDays -ne 2 -or $preview.policy.budgetBytes -ne 1024) { throw 'Retention parameter forwarding failed' }
    if ($before -ne (Get-FixtureSnapshot)) { throw 'Read-only CLI changed files or timestamps' }
    $maximum = Invoke-FixtureClean @('maximize','-WhatIf','--json','-Report',(Join-Path $fixtureRoot 'must-not-exist.json'))
    if ($maximum.reclaimableBytes -le 0 -or $maximum.deletedBytes -ne 0 -or $before -ne (Get-FixtureSnapshot)) { throw 'Maximize preview must be read-only' }
    $maximum = Invoke-FixtureClean @('maximize','--json')
    if ($maximum.failed.Count -ne 0 -or $maximum.deletedBytes -le 0 -or (Test-Path -LiteralPath (Join-Path $cacheRoot 'Engine'))) { throw 'Maximize CLI did not reclaim the build cache' }
    if (!(Test-Path -LiteralPath (Join-Path $cacheRoot 'sessions/pinned/.he-keep'))) { throw 'Maximize CLI removed pinned evidence' }
    Write-Output "unit-test: Windows PowerShell $($PSVersionTable.PSVersion); 12 script parsers + hectl CLI fixture: PASS"
}
finally {
    # Resolve and verify the exact temporary unit before recursive removal.
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $fixtureRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($fixtureRoot) -notmatch '^he-storage-cli-[0-9a-f]{32}$') {
        throw 'Refusing unsafe fixture cleanup'
    }
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}
