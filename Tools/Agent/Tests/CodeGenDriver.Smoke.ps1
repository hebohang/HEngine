[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    $directory = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-Sha256Text {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Get-Sha256File {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Get-OwnedTreeSnapshot {
    param(
        [string]$OutputRoot,
        [string]$ManifestPath
    )

    $manifest = [System.IO.File]::ReadAllText($ManifestPath) | ConvertFrom-Json
    $snapshot = @{}
    $snapshot['@manifest'] = [pscustomobject]@{
        path = $ManifestPath
        hash = Get-Sha256File -Path $ManifestPath
        ticks = [System.IO.File]::GetLastWriteTimeUtc($ManifestPath).Ticks
    }
    foreach ($output in @($manifest.outputs)) {
        $relative = [string]$output.path
        $path = Join-Path $OutputRoot $relative.Replace('/', '\')
        Assert-True ([System.IO.File]::Exists($path)) "owned snapshot output is missing: $relative"
        $snapshot[$relative] = [pscustomobject]@{
            path = $path
            hash = Get-Sha256File -Path $path
            ticks = [System.IO.File]::GetLastWriteTimeUtc($path).Ticks
        }
    }
    return $snapshot
}

function Assert-OwnedTreeSnapshot {
    param(
        [hashtable]$Expected,
        [string]$OutputRoot,
        [string]$ManifestPath,
        [string]$Label
    )

    $actual = Get-OwnedTreeSnapshot -OutputRoot $OutputRoot -ManifestPath $ManifestPath
    Assert-True ($actual.Count -eq $Expected.Count) "$Label changed the manifest-owned output count"
    foreach ($key in @($Expected.Keys)) {
        Assert-True ($actual.ContainsKey($key)) "$Label removed manifest-owned entry $key"
        Assert-True ($actual[$key].hash -eq $Expected[$key].hash) "$Label changed bytes for $key"
        Assert-True ($actual[$key].ticks -eq $Expected[$key].ticks) "$Label changed mtime for $key"
    }
}

function Invoke-Driver {
    param(
        [string]$Action,
        [string[]]$ExtraArguments = @()
    )

    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $script:DriverPath,
        '-Action', $Action,
        '-RepositoryRoot', $script:RepositoryRoot,
        '-ProjectRoot', $script:ProjectRoot,
        '-OutputRoot', $script:OutputRoot,
        '-ParserExecutable', $script:ParserExecutable,
        '-InputList', $script:InputList,
        '-ContextFile', $script:ContextFile,
        '-TemplateRoot', $script:TemplateRoot,
        '-Module', 'SmokeModule',
        '-LogPath', $script:LogPath)
    $arguments += $ExtraArguments
    $captured = @(& powershell.exe @arguments 2>&1)
    return [pscustomobject]@{
        exitCode = $LASTEXITCODE
        output = [object[]]$captured
        text = (@($captured | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }
}

$script:RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$script:DriverPath = Join-Path $script:RepositoryRoot 'Tools\Agent\CodeGenDriver.ps1'
$temporaryBase = [System.IO.Path]::GetTempPath().TrimEnd('\', '/')
$testRoot = Join-Path $temporaryBase ('HEngine-CodeGenDriver-Smoke-' + [guid]::NewGuid().ToString('N'))
$script:ProjectRoot = Join-Path $testRoot 'Project'
$script:OutputRoot = Join-Path $testRoot 'GeneratedSource'
$script:InputList = Join-Path $testRoot 'headers.list'
$script:ContextFile = Join-Path $testRoot 'context.txt'
$script:TemplateRoot = Join-Path $testRoot 'templates'
$script:ParserExecutable = Join-Path $testRoot 'FakeParser.cmd'
$fakeParserScript = Join-Path $testRoot 'FakeParser.ps1'
$script:LogPath = Join-Path $testRoot 'logs\codegen.log'
$resultPath = Join-Path $testRoot 'state\result.json'
$stampPath = Join-Path $testRoot 'state\content.stamp'

try {
    [void][System.IO.Directory]::CreateDirectory($script:ProjectRoot)
    [void][System.IO.Directory]::CreateDirectory($script:TemplateRoot)
    $headerA = Join-Path $script:ProjectRoot 'Source\Alpha\Foo.h'
    $headerB = Join-Path $script:ProjectRoot 'Source\Beta\Foo.h'
    Write-Utf8NoBom -Path $headerA -Text "struct AlphaFoo { int value; };`n"
    Write-Utf8NoBom -Path $headerB -Text "struct BetaFoo { float value; };`n"
    Write-Utf8NoBom -Path (Join-Path $script:TemplateRoot 'type.mustache') -Text "{{qualifiedName}}`n"
    Write-Utf8NoBom -Path $script:InputList -Text ($headerA + ';' + $headerB + ';')

    $fakeParser = @'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
$ErrorActionPreference = 'Stop'
$values = @{}
for ($index = 0; $index -lt $Arguments.Count; $index++) {
    $name = $Arguments[$index]
    if ($name -eq '--quiet' -or $name -eq '--validate-generated') { continue }
    if (-not $name.StartsWith('--') -or $index + 1 -ge $Arguments.Count) { throw "bad argument: $name" }
    $values[$name] = $Arguments[++$index]
}
foreach ($required in @('--input-list','--input-header','--output-root','--context','--templates','--index','--module')) {
    if (-not $values.ContainsKey($required)) { throw "missing $required" }
}
$context = [System.IO.File]::ReadAllText($values['--context'])
if ($context -match '(?m)^fail=true\s*$') {
    [Console]::Error.WriteLine('Fixture/Broken.h:7:3: error: requested fake parser failure')
    exit 17
}
$delayMilliseconds = 0
if ($context -match '(?m)^delayMs=([0-9]+)\s*$') {
    $delayMilliseconds = [int]$matches[1]
}
$parserStartTicks = [System.DateTime]::UtcNow.Ticks
[Console]::Out.WriteLine("FAKE_PARSER_WINDOW|start|$parserStartTicks")
if ($delayMilliseconds -gt 0) {
    Start-Sleep -Milliseconds $delayMilliseconds
}
[void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($values['--input-header']))
[System.IO.File]::WriteAllText($values['--input-header'], "// synthetic TU`n")
$headers = @([System.IO.File]::ReadAllText($values['--input-list']).Split(';') | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object)
$types = New-Object System.Collections.ArrayList
$outputs = New-Object System.Collections.ArrayList
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    foreach ($header in $headers) {
        $identityHash = ([System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($header.ToLowerInvariant())))).Replace('-','').ToLowerInvariant()
        $relative = '_generated/types/' + $identityHash.Substring(0, 12) + '-' + [System.IO.Path]::GetFileNameWithoutExtension($header) + '.gen.h'
        $destination = Join-Path $values['--output-root'] $relative.Replace('/','\')
        [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($destination))
        $body = "// generated from $header`n" + [System.IO.File]::ReadAllText($header)
        [System.IO.File]::WriteAllText($destination, $body, (New-Object System.Text.UTF8Encoding($false)))
        [void]$outputs.Add($relative)
        [void]$types.Add([pscustomobject][ordered]@{
            identity = 'fake:' + $identityHash
            usr = 'fake:' + $identityHash
            qualifiedName = [System.IO.Path]::GetFileNameWithoutExtension($header) + '::' + $identityHash.Substring(0, 4)
            legacyName = [System.IO.Path]::GetFileNameWithoutExtension($header)
            source = $header.Replace('\','/')
            line = 1
            column = 1
            outputs = @($relative)
            dependencies = @()
            fixtureExtra = 'preserved'
        })
    }
}
finally {
    $sha.Dispose()
}
$aggregate = '_generated/all.generated.h'
$aggregatePath = Join-Path $values['--output-root'] $aggregate.Replace('/','\')
[void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($aggregatePath))
[System.IO.File]::WriteAllText($aggregatePath, ((@($outputs.ToArray() | Sort-Object) -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
[void]$outputs.Add($aggregate)
$indexObject = [pscustomobject][ordered]@{
    schemaVersion = 1
    generatorVersion = '2'
    types = @($types.ToArray() | Sort-Object identity)
    outputs = @($outputs.ToArray() | Sort-Object)
}
$indexJson = ($indexObject | ConvertTo-Json -Depth 20 -Compress) + "`n"
[System.IO.File]::WriteAllText($values['--index'], $indexJson, (New-Object System.Text.UTF8Encoding($false)))
[Console]::Out.WriteLine("FAKE_PARSER_WINDOW|end|$([System.DateTime]::UtcNow.Ticks)")
exit 0
'@
    Write-Utf8NoBom -Path $fakeParserScript -Text $fakeParser
    $wrapper = "@echo off`r`n`"$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe`" -NoProfile -ExecutionPolicy Bypass -File `"%~dp0FakeParser.ps1`" %*`r`nexit /b %ERRORLEVEL%`r`n"
    Write-Utf8NoBom -Path $script:ParserExecutable -Text $wrapper

    $baseContext = @(
        'include=' + (Join-Path $script:ProjectRoot 'Source'),
        'define=SMOKE=1',
        'arg=-std=c++20',
        'sourceRoot=' + $script:ProjectRoot,
        'track=' + $fakeParserScript) -join "`n"
    Write-Utf8NoBom -Path $script:ContextFile -Text ($baseContext + "`n")

    $bootstrap = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json', '-ResultPath', $resultPath, '-StampPath', $stampPath)
    Assert-True ($bootstrap.exitCode -eq 0) "bootstrap failed: $($bootstrap.text)"
    $bootstrapJson = $bootstrap.text | ConvertFrom-Json
    Assert-True ($bootstrapJson.status -eq 'generated') 'bootstrap did not report generated'
    Assert-True ($bootstrapJson.changed -eq 3) 'bootstrap changed count was not 3'
    Assert-True ([System.IO.File]::Exists((Join-Path $script:OutputRoot '.he-codegen\manifest.json'))) 'manifest missing after bootstrap'
    $resultJson = [System.IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
    Assert-True ($resultJson.changed -eq 3) 'ResultPath did not preserve run statistics'
    Assert-True ([System.IO.File]::Exists($stampPath)) 'content StampPath was not written'
    $stampTimeBefore = [System.IO.File]::GetLastWriteTimeUtc($stampPath).Ticks

    $manifestPath = Join-Path $script:OutputRoot '.he-codegen\manifest.json'
    $manifestBefore = [System.IO.File]::ReadAllBytes($manifestPath)
    $manifestTimeBefore = [System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks
    $manifestObject = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    Assert-True (@($manifestObject.inputs | Where-Object { $_.kind -eq 'driver' }).Count -eq 1) 'driver implementation was not fingerprinted'
    $outputTimesBefore = @{}
    foreach ($output in @($manifestObject.outputs)) {
        $path = Join-Path $script:OutputRoot ([string]$output.path).Replace('/','\')
        $outputTimesBefore[[string]$output.path] = [System.IO.File]::GetLastWriteTimeUtc($path).Ticks
    }
    Assert-True ($manifestObject.types[0].fixtureExtra -eq 'preserved') 'type extension field was not preserved'

    $noop = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json', '-StampPath', $stampPath)
    Assert-True ($noop.exitCode -eq 0) "ensure no-op failed: $($noop.text)"
    $noopJson = $noop.text | ConvertFrom-Json
    Assert-True ($noopJson.status -eq 'clean') 'ensure no-op was not clean'
    Assert-True ($noopJson.changed -eq 0 -and $noopJson.deleted -eq 0) 'ensure no-op changed outputs'
    Assert-True ([System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks -eq $manifestTimeBefore) 'ensure no-op touched manifest mtime'
    Assert-True ([System.IO.File]::GetLastWriteTimeUtc($stampPath).Ticks -eq $stampTimeBefore) 'ensure no-op touched content stamp mtime'
    foreach ($output in @($manifestObject.outputs)) {
        $path = Join-Path $script:OutputRoot ([string]$output.path).Replace('/','\')
        Assert-True ([System.IO.File]::GetLastWriteTimeUtc($path).Ticks -eq $outputTimesBefore[[string]$output.path]) "ensure no-op touched $($output.path)"
    }

    $forcedNoop = Invoke-Driver -Action 'Run' -ExtraArguments @('--json')
    Assert-True ($forcedNoop.exitCode -eq 0) "forced no-op failed: $($forcedNoop.text)"
    $forcedNoopJson = $forcedNoop.text | ConvertFrom-Json
    Assert-True ($forcedNoopJson.changed -eq 0 -and $forcedNoopJson.deleted -eq 0) 'forced no-op changed outputs'
    Assert-True ([System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks -eq $manifestTimeBefore) 'forced no-op touched manifest mtime'

    $quiet = Invoke-Driver -Action 'Status' -ExtraArguments @('--quiet')
    Assert-True ($quiet.exitCode -eq 0) 'quiet status failed'
    Assert-True ($quiet.output.Count -eq 0) 'quiet status produced terminal output'

    Write-Utf8NoBom -Path $headerA -Text "struct AlphaFoo { int value; int next; };`n"
    $dirtyCheck = Invoke-Driver -Action 'Check' -ExtraArguments @('--json')
    Assert-True ($dirtyCheck.exitCode -eq 2) "dirty check exit was not 2: $($dirtyCheck.text)"
    $dirtyJson = $dirtyCheck.text | ConvertFrom-Json
    Assert-True ($dirtyJson.status -eq 'dirty') 'check did not report dirty'
    $incremental = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json')
    Assert-True ($incremental.exitCode -eq 0) "incremental ensure failed: $($incremental.text)"
    $incrementalJson = $incremental.text | ConvertFrom-Json
    Assert-True ($incrementalJson.changed -eq 1 -and $incrementalJson.deleted -eq 0) 'single header edit was not output-incremental'

    $canary = Join-Path $script:OutputRoot 'unowned-canary.txt'
    Write-Utf8NoBom -Path $canary -Text 'keep'
    Write-Utf8NoBom -Path $script:InputList -Text ($headerA + ';')
    $stale = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json')
    Assert-True ($stale.exitCode -eq 0) "stale cleanup failed: $($stale.text)"
    $staleJson = $stale.text | ConvertFrom-Json
    Assert-True ($staleJson.deleted -eq 1) 'stale cleanup did not delete exactly one owned output'
    Assert-True ([System.IO.File]::Exists($canary)) 'stale cleanup deleted an unowned canary'

    $validManifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
    $ownedBeforeFailure = @{}
    $validManifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    foreach ($output in @($validManifest.outputs)) {
        $path = Join-Path $script:OutputRoot ([string]$output.path).Replace('/','\')
        $ownedBeforeFailure[[string]$output.path] = [System.IO.File]::ReadAllBytes($path)
    }
    Write-Utf8NoBom -Path $script:ContextFile -Text ($baseContext + "`nfail=true`n")
    $failure = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json', '-ResultPath', $resultPath)
    Assert-True ($failure.exitCode -eq 1) 'fake parser failure did not fail driver'
    $failureJson = $failure.text | ConvertFrom-Json
    Assert-True ($failureJson.status -eq 'error') 'fake parser failure did not return error schema'
    Assert-True (([System.IO.File]::ReadAllBytes($manifestPath) -join ',') -eq ($validManifestBytes -join ',')) 'parser failure changed old manifest'
    foreach ($output in @($validManifest.outputs)) {
        $path = Join-Path $script:OutputRoot ([string]$output.path).Replace('/','\')
        Assert-True (([System.IO.File]::ReadAllBytes($path) -join ',') -eq ($ownedBeforeFailure[[string]$output.path] -join ',')) "parser failure changed old output $($output.path)"
    }
    $failureResult = [System.IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
    Assert-True ($failureResult.status -eq 'error') 'ResultPath did not capture failure schema'

    Write-Utf8NoBom -Path $script:ContextFile -Text ($baseContext + "`n")
    $final = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
    Assert-True ($final.exitCode -eq 0) "final status failed: $($final.text)"
    Assert-True (($final.text | ConvertFrom-Json).status -eq 'clean') 'restored context did not return to clean state'

    # Exercise actual partial publication followed by rollback at each I/O
    # boundary. Two changed outputs ensure the first file is already replaced
    # when the test-only fault fires on the second file.
    Write-Utf8NoBom -Path $script:InputList -Text ($headerA + ';' + $headerB + ';')
    $faultBaseline = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json')
    Assert-True ($faultBaseline.exitCode -eq 0) "fault baseline generation failed: $($faultBaseline.text)"
    $faultBaseHeaderA = [System.IO.File]::ReadAllText($headerA)
    $faultBaseHeaderB = [System.IO.File]::ReadAllText($headerB)
    $faultGateSnapshot = Get-OwnedTreeSnapshot -OutputRoot $script:OutputRoot -ManifestPath $manifestPath
    $previousFaultGate = $env:HE_CODEGEN_ENABLE_TEST_FAULTS
    try {
        Remove-Item Env:\HE_CODEGEN_ENABLE_TEST_FAULTS -ErrorAction SilentlyContinue
        $closedFaultGate = Invoke-Driver -Action 'Ensure' -ExtraArguments @(
            '-TestOnlyPublishFault', 'write', '--json')
    }
    finally {
        if ($null -ne $previousFaultGate) {
            $env:HE_CODEGEN_ENABLE_TEST_FAULTS = $previousFaultGate
        }
    }
    Assert-True ($closedFaultGate.exitCode -eq 1) 'test-only publish fault was accepted without its environment gate'
    Assert-OwnedTreeSnapshot -Expected $faultGateSnapshot -OutputRoot $script:OutputRoot `
        -ManifestPath $manifestPath -Label 'closed test-only fault gate'
    foreach ($faultStage in @('write', 'flush', 'rename')) {
        $faultSnapshot = Get-OwnedTreeSnapshot -OutputRoot $script:OutputRoot -ManifestPath $manifestPath
        Write-Utf8NoBom -Path $headerA -Text ($faultBaseHeaderA + "// fault-$faultStage-a`n")
        Write-Utf8NoBom -Path $headerB -Text ($faultBaseHeaderB + "// fault-$faultStage-b`n")

        $previousFaultSwitch = $env:HE_CODEGEN_ENABLE_TEST_FAULTS
        try {
            $env:HE_CODEGEN_ENABLE_TEST_FAULTS = '1'
            $faultResult = Invoke-Driver -Action 'Ensure' -ExtraArguments @(
                '-TestOnlyPublishFault', $faultStage, '--json')
        }
        finally {
            if ($null -eq $previousFaultSwitch) {
                Remove-Item Env:\HE_CODEGEN_ENABLE_TEST_FAULTS -ErrorAction SilentlyContinue
            }
            else {
                $env:HE_CODEGEN_ENABLE_TEST_FAULTS = $previousFaultSwitch
            }
        }

        Assert-True ($faultResult.exitCode -eq 1) "$faultStage publish fault did not fail the driver"
        $faultJson = $faultResult.text | ConvertFrom-Json
        Assert-True ($faultJson.status -eq 'error') "$faultStage publish fault did not return error schema"
        Assert-True ((@($faultJson.dirtyReasons) -join '|') -match "publish $faultStage failure") "$faultStage publish fault did not report its injected boundary"
        Assert-OwnedTreeSnapshot -Expected $faultSnapshot -OutputRoot $script:OutputRoot `
            -ManifestPath $manifestPath -Label "$faultStage publish rollback"
        Assert-True ([System.IO.File]::Exists($canary)) "$faultStage publish rollback removed the unowned canary"

        Write-Utf8NoBom -Path $headerA -Text $faultBaseHeaderA
        Write-Utf8NoBom -Path $headerB -Text $faultBaseHeaderB
        $faultRestored = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
        Assert-True ($faultRestored.exitCode -eq 0 -and
            ($faultRestored.text | ConvertFrom-Json).status -eq 'clean') "$faultStage fixture did not restore clean input state"
    }

    # Two independent output roots must be able to hold their own locks and
    # execute parser windows concurrently.
    $parallelContext = Join-Path $testRoot 'parallel-context.txt'
    Write-Utf8NoBom -Path $parallelContext -Text ($baseContext + "`ndelayMs=1500`n")
    $parallelOutputA = Join-Path $testRoot 'ParallelGeneratedA'
    $parallelOutputB = Join-Path $testRoot 'ParallelGeneratedB'
    $parallelLogA = Join-Path $testRoot 'logs\parallel-a.log'
    $parallelLogB = Join-Path $testRoot 'logs\parallel-b.log'
    $parallelStdoutA = Join-Path $testRoot 'logs\parallel-a.stdout.log'
    $parallelStdoutB = Join-Path $testRoot 'logs\parallel-b.stdout.log'
    $parallelStderrA = Join-Path $testRoot 'logs\parallel-a.stderr.log'
    $parallelStderrB = Join-Path $testRoot 'logs\parallel-b.stderr.log'
    $parallelResultA = Join-Path $testRoot 'state\parallel-a.json'
    $parallelResultB = Join-Path $testRoot 'state\parallel-b.json'
    [void][System.IO.Directory]::CreateDirectory((Join-Path $testRoot 'logs'))
    $parallelBaseArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:DriverPath,
        '-Action', 'Ensure', '-RepositoryRoot', $script:RepositoryRoot,
        '-ProjectRoot', $script:ProjectRoot,
        '-ParserExecutable', $script:ParserExecutable,
        '-InputList', $script:InputList, '-ContextFile', $parallelContext,
        '-TemplateRoot', $script:TemplateRoot, '-Module', 'ParallelSmoke', '--json')
    $parallelArgumentsA = @($parallelBaseArguments + @('-OutputRoot', $parallelOutputA, '-LogPath', $parallelLogA, '-ResultPath', $parallelResultA))
    $parallelArgumentsB = @($parallelBaseArguments + @('-OutputRoot', $parallelOutputB, '-LogPath', $parallelLogB, '-ResultPath', $parallelResultB))
    $parallelProcessA = Start-Process -FilePath powershell.exe -ArgumentList $parallelArgumentsA `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $parallelStdoutA -RedirectStandardError $parallelStderrA
    $parallelProcessB = Start-Process -FilePath powershell.exe -ArgumentList $parallelArgumentsB `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $parallelStdoutB -RedirectStandardError $parallelStderrB
    $parallelProcessA.WaitForExit()
    $parallelProcessB.WaitForExit()
    $parallelProcessA.Refresh()
    $parallelProcessB.Refresh()
    Assert-True ($parallelProcessA.HasExited -and $parallelProcessB.HasExited) 'parallel driver processes did not exit'
    Assert-True ([System.IO.File]::Exists($parallelResultA)) "parallel output A result is missing: stdout=$([System.IO.File]::ReadAllText($parallelStdoutA)) stderr=$([System.IO.File]::ReadAllText($parallelStderrA))"
    Assert-True ([System.IO.File]::Exists($parallelResultB)) "parallel output B result is missing: stdout=$([System.IO.File]::ReadAllText($parallelStdoutB)) stderr=$([System.IO.File]::ReadAllText($parallelStderrB))"
    $parallelResultJsonA = [System.IO.File]::ReadAllText($parallelResultA) | ConvertFrom-Json
    $parallelResultJsonB = [System.IO.File]::ReadAllText($parallelResultB) | ConvertFrom-Json
    Assert-True ($parallelResultJsonA.status -eq 'generated') "parallel output A failed: $([System.IO.File]::ReadAllText($parallelStdoutA))"
    Assert-True ($parallelResultJsonB.status -eq 'generated') "parallel output B failed: $([System.IO.File]::ReadAllText($parallelStdoutB))"
    Assert-True ((Get-Item -LiteralPath $parallelStderrA).Length -eq 0) 'parallel output A wrote stderr'
    Assert-True ((Get-Item -LiteralPath $parallelStderrB).Length -eq 0) 'parallel output B wrote stderr'
    Assert-True ([System.IO.File]::Exists((Join-Path $parallelOutputA '.he-codegen\manifest.json'))) 'parallel output A manifest is missing'
    Assert-True ([System.IO.File]::Exists((Join-Path $parallelOutputB '.he-codegen\manifest.json'))) 'parallel output B manifest is missing'
    $parallelLogTextA = Get-Content -LiteralPath $parallelLogA -Raw
    $parallelLogTextB = Get-Content -LiteralPath $parallelLogB -Raw
    $startMatchA = [regex]::Match($parallelLogTextA, 'FAKE_PARSER_WINDOW\|start\|([0-9]+)')
    $endMatchA = [regex]::Match($parallelLogTextA, 'FAKE_PARSER_WINDOW\|end\|([0-9]+)')
    $startMatchB = [regex]::Match($parallelLogTextB, 'FAKE_PARSER_WINDOW\|start\|([0-9]+)')
    $endMatchB = [regex]::Match($parallelLogTextB, 'FAKE_PARSER_WINDOW\|end\|([0-9]+)')
    Assert-True ($startMatchA.Success -and $endMatchA.Success -and
        $startMatchB.Success -and $endMatchB.Success) 'parallel parser timing markers are incomplete'
    $parallelStartA = [int64]$startMatchA.Groups[1].Value
    $parallelEndA = [int64]$endMatchA.Groups[1].Value
    $parallelStartB = [int64]$startMatchB.Groups[1].Value
    $parallelEndB = [int64]$endMatchB.Groups[1].Value
    Assert-True ($parallelStartA -lt $parallelEndB -and $parallelStartB -lt $parallelEndA) 'different OutputRoot parser windows did not overlap'

    $schemaManifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)
    $schemaManifestTicks = [System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks
    $schemaManifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $schemaManifest.schemaVersion = 99
    Write-Utf8NoBom -Path $manifestPath -Text (($schemaManifest | ConvertTo-Json -Depth 100 -Compress) + "`n")
    $futureSchema = Invoke-Driver -Action 'Ensure' -ExtraArguments @('--json')
    Assert-True ($futureSchema.exitCode -ne 0) 'future manifest schema was silently downgraded'
    [System.IO.File]::WriteAllBytes($manifestPath, $schemaManifestBytes)
    [System.IO.File]::SetLastWriteTimeUtc($manifestPath, (New-Object System.DateTime($schemaManifestTicks, [System.DateTimeKind]::Utc)))
    $restoredSchema = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
    Assert-True ($restoredSchema.exitCode -eq 0 -and ($restoredSchema.text | ConvertFrom-Json).status -eq 'clean') 'manifest schema fixture did not restore clean state'

    $scopeHash = (Get-Sha256Text -Text ($script:OutputRoot.Replace('\','/').ToLowerInvariant())).Substring(0, 16)
    $scopePrefix = '.hcg-' + $scopeHash.Substring(0, 12)
    $scopeParent = [System.IO.Path]::GetDirectoryName($script:OutputRoot)
    $lockPath = Join-Path $scopeParent ($scopePrefix + '.lock')
    $heldLock = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $locked = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
        Assert-True ($locked.exitCode -eq 1) 'same-output-root lock contention did not fail'
        Assert-True (($locked.text | ConvertFrom-Json).status -eq 'error') 'lock contention did not return error schema'
    }
    finally {
        $heldLock.Dispose()
    }

    $recoveryManifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $recoveryRelative = [string]$recoveryManifest.outputs[0].path
    $recoveryOutput = Join-Path $script:OutputRoot $recoveryRelative.Replace('/','\')
    $recoveryBytes = [System.IO.File]::ReadAllBytes($recoveryOutput)
    $recoveryHash = Get-Sha256File -Path $recoveryOutput
    $recoveryLastWriteTimeUtcTicks = [System.IO.File]::GetLastWriteTimeUtc($recoveryOutput).Ticks
    $recoveryId = [guid]::NewGuid().ToString('N')
    $stagingRoot = Join-Path $scopeParent ($scopePrefix + '.s-' + $recoveryId.Substring(0, 8))
    $backupRoot = Join-Path $scopeParent ($scopePrefix + '.b-' + $recoveryId.Substring(0, 8))
    [void][System.IO.Directory]::CreateDirectory($stagingRoot)
    $backupOutput = Join-Path $backupRoot $recoveryRelative.Replace('/','\')
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($backupOutput))
    [System.IO.File]::WriteAllBytes($backupOutput, $recoveryBytes)
    Write-Utf8NoBom -Path $recoveryOutput -Text 'interrupted publish content'
    $journalPath = Join-Path $scopeParent ($scopePrefix + '.journal.json')
    $journal = [pscustomobject][ordered]@{
        schemaVersion = 1
        outputRoot = $script:OutputRoot.Replace('\','/')
        phase = 'publishing'
        stagingRoot = $stagingRoot.Replace('\','/')
        backupRoot = $backupRoot.Replace('\','/')
        changed = @([pscustomobject][ordered]@{
            path = $recoveryRelative
            hadOriginal = $true
            originalHash = $recoveryHash
            originalLastWriteTimeUtcTicks = $recoveryLastWriteTimeUtcTicks
        })
        deleted = @()
        manifestChanged = $false
        manifestHadOriginal = $true
        manifestOriginalHash = Get-Sha256File -Path $manifestPath
        manifestOriginalLastWriteTimeUtcTicks = [System.IO.File]::GetLastWriteTimeUtc($manifestPath).Ticks
    }
    Write-Utf8NoBom -Path $journalPath -Text (($journal | ConvertTo-Json -Depth 10 -Compress) + "`n")
    $recovered = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
    Assert-True ($recovered.exitCode -eq 0) "journal recovery failed: $($recovered.text)"
    Assert-True (($recovered.text | ConvertFrom-Json).status -eq 'clean') 'journal recovery did not restore clean state'
    Assert-True ((Get-Sha256File -Path $recoveryOutput) -eq $recoveryHash) 'journal recovery did not restore original output'
    Assert-True ([System.IO.File]::GetLastWriteTimeUtc($recoveryOutput).Ticks -eq $recoveryLastWriteTimeUtcTicks) 'journal recovery did not restore original output mtime'
    Assert-True (-not [System.IO.File]::Exists($journalPath)) 'journal recovery left the journal behind'
    Assert-True (-not [System.IO.Directory]::Exists($stagingRoot)) 'journal recovery left staging behind'
    Assert-True (-not [System.IO.Directory]::Exists($backupRoot)) 'journal recovery left backup behind'

    $temporaryNameCanary = Join-Path $script:OutputRoot '.he-codegen-write-user-owned.tmp'
    Write-Utf8NoBom -Path $temporaryNameCanary -Text 'not driver owned'
    $temporaryCleanup = Invoke-Driver -Action 'Status' -ExtraArguments @('--json')
    Assert-True ($temporaryCleanup.exitCode -eq 0) 'temporary cleanup status failed'
    Assert-True ([System.IO.File]::Exists($temporaryNameCanary)) 'temporary cleanup deleted an imprecisely named canary'

    $sourceCanary = Join-Path $script:RepositoryRoot 'Tools\Agent\Tests\.codegen-driver-safety-canary.tmp'
    Write-Utf8NoBom -Path $sourceCanary -Text 'preserve source canary'
    $sourceCanaryHash = Get-Sha256File -Path $sourceCanary
    $protectedArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:DriverPath,
        '-Action', 'Status', '-RepositoryRoot', $script:RepositoryRoot,
        '-ProjectRoot', $script:ProjectRoot,
        '-OutputRoot', (Join-Path $script:RepositoryRoot 'Engine\Source\Runtime'),
        '-ParserExecutable', $script:ParserExecutable,
        '-InputList', $script:InputList, '-ContextFile', $script:ContextFile,
        '-TemplateRoot', $script:TemplateRoot, '-Module', 'SmokeModule',
        '-LogPath', $script:LogPath, '--json')
    $protectedOutput = @(& powershell.exe @protectedArguments 2>&1)
    Assert-True ($LASTEXITCODE -ne 0) 'protected source OutputRoot was accepted'
    Assert-True ((Get-Sha256File -Path $sourceCanary) -eq $sourceCanaryHash) 'protected OutputRoot changed source canary'

    $badLogArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:DriverPath,
        '-Action', 'Status', '-RepositoryRoot', $script:RepositoryRoot,
        '-ProjectRoot', $script:ProjectRoot, '-OutputRoot', $script:OutputRoot,
        '-ParserExecutable', $script:ParserExecutable,
        '-InputList', $script:InputList, '-ContextFile', $script:ContextFile,
        '-TemplateRoot', $script:TemplateRoot, '-Module', 'SmokeModule',
        '-LogPath', $sourceCanary, '--json')
    $badLogOutput = @(& powershell.exe @badLogArguments 2>&1)
    Assert-True ($LASTEXITCODE -ne 0) 'source-tree LogPath was accepted'
    Assert-True ((Get-Sha256File -Path $sourceCanary) -eq $sourceCanaryHash) 'source-tree LogPath changed source canary'
    $badResult = Invoke-Driver -Action 'Status' -ExtraArguments @('-ResultPath', $sourceCanary, '--json')
    Assert-True ($badResult.exitCode -ne 0) 'source-tree ResultPath was accepted'
    Assert-True ((Get-Sha256File -Path $sourceCanary) -eq $sourceCanaryHash) 'source-tree ResultPath changed source canary'
    [System.IO.File]::Delete($sourceCanary)

    [Console]::Out.WriteLine('CodeGenDriver smoke: PASS (bootstrap, deterministic no-op, incremental update, owned stale cleanup, parser/publish failure rollback, json/quiet/result, same-root lock, different-root parallelism, interrupted-journal recovery, protected roots)')
}
finally {
    $sourceCanary = Join-Path $script:RepositoryRoot 'Tools\Agent\Tests\.codegen-driver-safety-canary.tmp'
    if ([System.IO.File]::Exists($sourceCanary)) {
        [System.IO.File]::Delete($sourceCanary)
    }
    $fullTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $fullTemporaryBase = [System.IO.Path]::GetFullPath($temporaryBase).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($fullTestRoot.StartsWith($fullTemporaryBase, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($fullTestRoot).StartsWith('HEngine-CodeGenDriver-Smoke-', [System.StringComparison]::Ordinal)) {
        if ([System.IO.Directory]::Exists($fullTestRoot)) {
            Remove-Item -LiteralPath $fullTestRoot -Recurse -Force
        }
    }
}
