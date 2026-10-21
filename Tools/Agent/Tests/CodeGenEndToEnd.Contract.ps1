[CmdletBinding()]
param([string]$ParserExecutable = "")

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:PowerShellExecutable = (Get-Process -Id $PID).Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$driver = Join-Path $repositoryRoot "Tools\Agent\CodeGenDriver.ps1"
$fixtureSource = Join-Path $repositoryRoot "Engine\Source\Programs\HEngineParser\Tests\Fixtures\Contract"
$templateSource = Join-Path $repositoryRoot "Engine\Source\Programs\HEngineParser\Template"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Write-Utf8([string]$Path, [string]$Text) {
    $parent = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($parent)) { [void][System.IO.Directory]::CreateDirectory($parent) }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}
function Snapshot-Owned([string]$ManifestPath, [string]$OutputRoot) {
    $manifest = [System.IO.File]::ReadAllText($ManifestPath) | ConvertFrom-Json
    $snapshot = @{}
    foreach ($output in @($manifest.outputs)) {
        $path = Join-Path $OutputRoot ([string]$output.path).Replace('/', '\')
        $snapshot[[string]$output.path] = [pscustomobject]@{
            hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            ticks = [System.IO.File]::GetLastWriteTimeUtc($path).Ticks
        }
    }
    return $snapshot
}
function Invoke-Driver([string]$Action) {
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $driver,
        '-Action', $Action, '-RepositoryRoot', $repositoryRoot,
        '-ProjectRoot', $script:ProjectRoot, '-OutputRoot', $script:OutputRoot,
        '-ParserExecutable', $script:Parser, '-InputList', $script:InputList,
        '-ContextFile', $script:Context, '-TemplateRoot', $script:TemplateRoot,
        '-Module', 'EndToEnd', '-LogPath', $script:LogPath, '--json')
    $output = @(& $script:PowerShellExecutable @arguments 2>&1)
    return [pscustomobject]@{
        exitCode = $LASTEXITCODE
        text = (@($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }
}

if ([string]::IsNullOrWhiteSpace($ParserExecutable)) {
    $candidate = Get-ChildItem -LiteralPath (Join-Path $repositoryRoot "out\agent") `
        -Filter HEngineParser.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch "[\\/]CMakeFiles[\\/]" } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $candidate) { throw "Build HEngineParser before this contract." }
    $ParserExecutable = $candidate.FullName
}
$script:Parser = [System.IO.Path]::GetFullPath($ParserExecutable)

$temporaryBase = [System.IO.Path]::GetTempPath().TrimEnd('\', '/')
if (($PSVersionTable.PSEdition -eq 'Core') -and $IsMacOS) {
    # macOS exposes /var through a system symlink; use its physical path so
    # the transaction's prohibition on reparse points remains effective.
    $physicalVar = [System.IO.DirectoryInfo]::new('/var').ResolveLinkTarget($true).FullName
    if ($temporaryBase.StartsWith('/var/')) {
        $temporaryBase = $physicalVar + $temporaryBase.Substring(4)
    }
}
$testRoot = Join-Path $temporaryBase ("HEngine-CodeGen-E2E-" + [guid]::NewGuid().ToString("N"))
try {
    $fixture = Join-Path $testRoot "Fixture"
    Copy-Item -LiteralPath $fixtureSource -Destination $fixture -Recurse
    $script:ProjectRoot = Join-Path $testRoot "Project"
    [void][System.IO.Directory]::CreateDirectory($script:ProjectRoot)
    Write-Utf8 -Path (Join-Path $script:ProjectRoot "Project.heproject") -Text `
        ('{"ProjectId":"fixture-external","ExecutableName":"FixtureGame"}' + "`n")
    $script:TemplateRoot = Join-Path $testRoot "Templates"
    Copy-Item -LiteralPath $templateSource -Destination $script:TemplateRoot -Recurse
    $script:OutputRoot = Join-Path $testRoot "GeneratedSource"
    $script:InputList = Join-Path $fixture "input-list.txt"
    $script:Context = Join-Path $fixture "context.txt"
    $script:LogPath = Join-Path $testRoot "logs\codegen.log"
    $aHeader = Join-Path $fixture "A\Foo.h"
    $bHeader = Join-Path $fixture "B\Foo.h"
    $typesHeader = Join-Path $fixture "Model\Types.h"
    Write-Utf8 -Path $script:InputList -Text (($typesHeader, $bHeader, $aHeader) -join ';' | ForEach-Object { $_ + ";`n" })
    Write-Utf8 -Path $script:Context -Text ("host=Editor`ndistribution=Development`npluginIds=fixture.editor`ninclude=$($fixture.Replace('\','/'))`nsourceRoot=$($fixture.Replace('\','/'))|fixture`n")

    $bootstrap = Invoke-Driver -Action "Ensure"
    Assert-True ($bootstrap.exitCode -eq 0) "bootstrap failed: $($bootstrap.text)"
    $bootstrapJson = $bootstrap.text | ConvertFrom-Json
    Assert-True ($bootstrapJson.status -eq "generated") "bootstrap did not generate"
    $manifestPath = Join-Path $script:OutputRoot ".he-codegen\manifest.json"
    $manifestTime = [System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks
    $beforeNoop = Snapshot-Owned -ManifestPath $manifestPath -OutputRoot $script:OutputRoot

    $noop = Invoke-Driver -Action "Ensure"
    Assert-True ($noop.exitCode -eq 0) "no-op failed: $($noop.text)"
    $noopJson = $noop.text | ConvertFrom-Json
    Assert-True ($noopJson.status -eq "clean" -and $noopJson.changed -eq 0 -and $noopJson.deleted -eq 0) "no-op was not clean"
    Assert-True ([System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks -eq $manifestTime) "no-op touched manifest mtime"
    foreach ($relative in $beforeNoop.Keys) {
        $path = Join-Path $script:OutputRoot $relative.Replace('/', '\')
        Assert-True ((Get-FileHash $path -Algorithm SHA256).Hash -eq $beforeNoop[$relative].hash) "no-op changed $relative"
        Assert-True ([System.IO.File]::GetLastWriteTimeUtc($path).Ticks -eq $beforeNoop[$relative].ticks) "no-op touched $relative mtime"
    }

    $commonHeader = Join-Path $fixture "Common.h"
    $indirectHeader = Join-Path $fixture "Indirect.h"
    Write-Utf8 -Path $indirectHeader -Text "#pragma once`ninline constexpr int DependencyProbe = 1;`n"
    Write-Utf8 -Path $commonHeader -Text ([IO.File]::ReadAllText($commonHeader) + "`n#include `"Indirect.h`"`n")
    $discovered = Invoke-Driver -Action "Ensure"
    Assert-True ($discovered.exitCode -eq 0) "include closure discovery failed: $($discovered.text)"
    $dependencyManifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    Assert-True (@($dependencyManifest.inputs | Where-Object {$_.kind -eq 'include' -and $_.path -match 'Indirect[.]h$'}).Count -eq 1) "transitive include not recorded"
    Write-Utf8 -Path $indirectHeader -Text "#pragma once`ninline constexpr int DependencyProbe = 2;`n"
    $dependentUpdate = Invoke-Driver -Action "Ensure"
    Assert-True ($dependentUpdate.exitCode -eq 0) "transitive include update failed: $($dependentUpdate.text)"
    $dependentJson = $dependentUpdate.text | ConvertFrom-Json
    Assert-True ($dependentJson.status -eq 'generated') "transitive include change did not invalidate codegen"
    Assert-True ($dependentJson.changed -eq 0) "unrelated constant rewrote generated content"

    $unrelatedSource = Join-Path $testRoot "Unrelated.cpp"
    Write-Utf8 -Path $unrelatedSource -Text "int UnrelatedCodegenContract() { return 42; }`n"
    $unrelated = Invoke-Driver -Action "Ensure"
    Assert-True ($unrelated.exitCode -eq 0) "unrelated source no-op failed: $($unrelated.text)"
    $unrelatedJson = $unrelated.text | ConvertFrom-Json
    Assert-True ($unrelatedJson.status -eq "clean") "unrelated .cpp invalidated codegen freshness"

    $aText = [System.IO.File]::ReadAllText($aHeader)
    Write-Utf8 -Path $aHeader -Text ($aText.Replace("int m_value = 1;", "int m_value = 1; int m_added = 3;"))
    $incremental = Invoke-Driver -Action "Ensure"
    Assert-True ($incremental.exitCode -eq 0) "single-field update failed: $($incremental.text)"
    $incrementalJson = $incremental.text | ConvertFrom-Json
    Assert-True ($incrementalJson.changed -ge 1 -and $incrementalJson.changed -le 3) "single-field update rewrote an unexpected number of outputs"

    $outsideCanary = Join-Path $testRoot "outside-canary.txt"
    Write-Utf8 -Path $outsideCanary -Text "keep"
    $typesText = [System.IO.File]::ReadAllText($typesHeader)
    $typesText = $typesText.Replace(('#include "B/Foo.h"' + "`r`n"), "").Replace(('#include "B/Foo.h"' + "`n"), "")
    $typesText = $typesText.Replace("        std::vector<Beta::Foo> m_items;`r`n", "").Replace("        std::vector<Beta::Foo> m_items;`n", "")
    Write-Utf8 -Path $typesHeader -Text $typesText
    [System.IO.File]::Delete($bHeader)
    Write-Utf8 -Path $script:InputList -Text (($typesHeader, $aHeader) -join ';' | ForEach-Object { $_ + ";`n" })
    $stale = Invoke-Driver -Action "Ensure"
    Assert-True ($stale.exitCode -eq 0) "stale cleanup failed: $($stale.text)"
    $staleJson = $stale.text | ConvertFrom-Json
    Assert-True ($staleJson.deleted -eq 3) "removed reflected header did not delete exactly three canonical outputs"
    Assert-True ([System.IO.File]::ReadAllText($outsideCanary) -eq "keep") "stale cleanup touched root-external canary"

    $validManifest = [System.IO.File]::ReadAllBytes($manifestPath)
    $validOutputs = Snapshot-Owned -ManifestPath $manifestPath -OutputRoot $script:OutputRoot
    Write-Utf8 -Path $aHeader -Text "#pragma once`nthis is not valid c++;`n"
    $failure = Invoke-Driver -Action "Ensure"
    Assert-True ($failure.exitCode -ne 0) "syntax failure unexpectedly succeeded"
    Assert-True (([System.IO.File]::ReadAllBytes($manifestPath) -join ',') -eq ($validManifest -join ',')) "syntax failure changed manifest"
    foreach ($relative in $validOutputs.Keys) {
        $path = Join-Path $script:OutputRoot $relative.Replace('/', '\')
        Assert-True ((Get-FileHash $path -Algorithm SHA256).Hash -eq $validOutputs[$relative].hash) "syntax failure changed $relative"
        Assert-True ([System.IO.File]::GetLastWriteTimeUtc($path).Ticks -eq $validOutputs[$relative].ticks) "syntax failure touched $relative mtime"
    }

    Write-Utf8 -Path $aHeader -Text ($aText.Replace("int m_value = 1;", "int m_value = 1; int m_added = 3;"))
    $oldIdentity = (Get-Content $manifestPath -Raw | ConvertFrom-Json).configIdentity
    $contextText = [System.IO.File]::ReadAllText($script:Context).Replace("host=Editor", "host=Tests")
    Write-Utf8 -Path $script:Context -Text $contextText
    $hostChange = Invoke-Driver -Action "Ensure"
    Assert-True ($hostChange.exitCode -eq 0) "host identity regeneration failed: $($hostChange.text)"
    $hostJson = $hostChange.text | ConvertFrom-Json
    Assert-True ($hostJson.configIdentity -ne $oldIdentity) "host change reused old config identity"

    $hostIdentity = $hostJson.configIdentity
    Write-Utf8 -Path $script:Context -Text `
        ([System.IO.File]::ReadAllText($script:Context).Replace("pluginIds=fixture.editor", "pluginIds=fixture.tests"))
    $pluginChange = Invoke-Driver -Action "Ensure"
    Assert-True ($pluginChange.exitCode -eq 0) "plugin identity regeneration failed: $($pluginChange.text)"
    $pluginJson = $pluginChange.text | ConvertFrom-Json
    Assert-True ($pluginJson.configIdentity -ne $hostIdentity) "active plugin change reused old config identity"

    $pluginIdentity = $pluginJson.configIdentity
    $secondProjectRoot = Join-Path $testRoot "SecondProject"
    [void][System.IO.Directory]::CreateDirectory($secondProjectRoot)
    Write-Utf8 -Path (Join-Path $secondProjectRoot "Project.heproject") -Text `
        ('{"ProjectId":"fixture-second","ExecutableName":"SecondGame"}' + "`n")
    $script:ProjectRoot = $secondProjectRoot
    $projectChange = Invoke-Driver -Action "Ensure"
    Assert-True ($projectChange.exitCode -eq 0) "external project identity regeneration failed: $($projectChange.text)"
    $projectJson = $projectChange.text | ConvertFrom-Json
    Assert-True ($projectJson.configIdentity -ne $pluginIdentity) "external Project.heproject change reused old identity"

    [Console]::Out.WriteLine(
        "Codegen end-to-end: PASS (fieldChanged=$($incrementalJson.changed), staleDeleted=$($staleJson.deleted), real parser bootstrap/no-op, transitive include discovery/invalidation, syntax rollback, unrelated-source no-op, host/plugin/external-project identity)")
}
finally {
    $fullTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $allowedPrefix = [System.IO.Path]::GetFullPath($temporaryBase).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($fullTestRoot.StartsWith($allowedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($fullTestRoot).StartsWith("HEngine-CodeGen-E2E-", [System.StringComparison]::Ordinal)) {
        if ([System.IO.Directory]::Exists($fullTestRoot)) { Remove-Item -LiteralPath $fullTestRoot -Recurse -Force }
    }
}
