$ErrorActionPreference = 'Stop'

# Import exactly the production function. Do not dot-source Session.ps1: its
# top-level dispatcher and active-session paths must never run in this test.
$parseTokens = $null
$parseErrors = $null
$sourcePath = Join-Path $PSScriptRoot 'Session.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Session.ps1 failed AST parsing.' }
$functions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Stop-AgentSession'
}, $true))
if ($functions.Count -ne 1) { throw 'Expected exactly one Stop-AgentSession definition.' }

function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "VFX session stop regression: $Message" }
}
function Get-RecordedProcess { param($Owner) return $script:mockProcess }
function Stop-RecordedSessionJob {
    param($Owner)
    $script:events.Add('job')
    return $false
}
function Set-RecordedSessionState {
    param($Owner, [string]$State)
    $Owner.state = $State
    $script:events.Add('state:' + $State)
}
function Complete-HEStorageRun {
    param($Run, [string]$State, [switch]$NoAuto)
    Assert-Test ($Run.Id -eq 'mock-storage' -and -not $Run.Outer -and $NoAuto) 'Storage owner/NoAuto contract changed.'
    $script:storageStates.Add($State)
    $script:events.Add('storage:' + $State)
}
function Invoke-AgentCall {
    param($Owner, [string]$CallMethod, [string]$CallParamsJson, [switch]$Quiet)
    Assert-Test ($CallMethod -eq 'session.stop' -and $CallParamsJson -eq '{}' -and $Quiet) 'Unexpected Agent request.'
    $script:events.Add('rpc')
    return [pscustomobject]@{ ok = $true }
}
function Stop-AgentOwnedProcess { param($Process, $Description) throw 'Unexpected owned-process termination in mock test.' }
function Stop-Process { throw 'Real process termination is forbidden in this test.' }
function Get-Process { throw 'Real process enumeration is forbidden in this test.' }

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('he-vfx-stop-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot
$ownedFiles = [Collections.Generic.List[string]]::new()
$results = [Collections.Generic.List[object]]::new()
# A real script file retains PSScriptRoot in Windows PowerShell 5.1; an
# Invoke-Expression definition would lose it. Copy the complete production
# validation helper beside the extracted function, preserving its real dot-source.
$functionFixture = Join-Path $testRoot 'Extracted-Stop.ps1'
$validationFixture = Join-Path $testRoot 'Rendering-Validation.ps1'
[IO.File]::WriteAllText($functionFixture, $functions[0].Extent.Text, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Rendering-Validation.ps1') -Destination $validationFixture
$ownedFiles.Add($functionFixture); $ownedFiles.Add($validationFixture)
. $functionFixture
function Test-Case {
    param([string]$Name, [bool]$Gone, $ExitCode, [string]$Log,
        [bool]$MustThrow, [string]$ExpectedStorage, [int]$Repeats = 1)
    $script:events = [Collections.Generic.List[string]]::new()
    $script:storageStates = [Collections.Generic.List[string]]::new()
    $script:mockProcess = $null
    if (-not $Gone) {
        $script:mockProcess = [pscustomobject]@{ ExitCode = $ExitCode }
        $script:mockProcess | Add-Member ScriptProperty Handle {
            $script:events.Add('handle')
            return [IntPtr]42
        }
        $script:mockProcess | Add-Member ScriptMethod WaitForExit {
            param($Milliseconds)
            $script:events.Add('wait')
            if ($null -ne $Milliseconds) { return $true }
        }
        $script:mockProcess | Add-Member ScriptMethod Refresh { $script:events.Add('refresh') }
    }
    $stdout = Join-Path $testRoot ($Name + '.stdout.log')
    $stderr = Join-Path $testRoot ($Name + '.stderr.log')
    [IO.File]::WriteAllText($stdout, $Log, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderr, '', [Text.UTF8Encoding]::new($false))
    $ownedFiles.Add($stdout); $ownedFiles.Add($stderr)
    $owner = [pscustomobject]@{ pid = 424242; state = 'running'; storageId = 'mock-storage'
        renderValidation = $true; stdoutLog = $stdout; stderrLog = $stderr }
    for ($index = 0; $index -lt $Repeats; $index++) {
        $thrown = $null
        try { Stop-AgentSession -Owner $owner | Out-Null }
        catch { $thrown = $_.Exception.Message }
        Assert-Test (($null -ne $thrown) -eq $MustThrow) "$Name exception mismatch: $thrown"
        if ($MustThrow) {
            $pattern = if ($Log -ne 'clean log') { 'Rendering validation reported' } else { 'exited unsuccessfully' }
            Assert-Test ($thrown -match $pattern) "$Name failed for an unrelated reason: $thrown"
        }
        Assert-Test ($owner.state -eq 'stopped') "$Name did not record stopped state."
    }
    Assert-Test ($script:storageStates.Count -eq $Repeats) "$Name did not register every stop result."
    Assert-Test (@($script:storageStates | Where-Object { $_ -ne $ExpectedStorage }).Count -eq 0) "$Name Storage state mismatch."
    if ($Gone) {
        Assert-Test (-not $script:events.Contains('rpc')) "$Name sent an RPC to a gone process."
    } else {
        Assert-Test ($script:events[0] -eq 'handle') "$Name did not pin the native handle before stop."
        Assert-Test (@($script:events | Where-Object { $_ -eq 'wait' }).Count -eq 2) "$Name did not complete both waits."
        if ($Log -eq 'clean log') { Assert-Test ($script:events.Contains('refresh')) "$Name did not refresh exit status." }
    }
    $results.Add([pscustomobject]@{ name = $Name; status = 'PASS'; throws = $MustThrow; storage = $ExpectedStorage; repeats = $Repeats })
}

try {
    Test-Case 'gone-validation-error' $true $null 'NVRHI validation error: injected' $true 'unknown'
    Test-Case 'gone-device-lost' $true $null 'NVRHI Vulkan: Device Removed VK_ERROR_DEVICE_LOST' $true 'unknown'
    Test-Case 'gone-clean-idempotent' $true $null 'clean log' $false 'unknown' 2
    Test-Case 'live-exit-zero' $false 0 'clean log' $false 'success'
    Test-Case 'live-exit-nonzero' $false 23 'clean log' $true 'failed'
    Test-Case 'live-exit-unavailable' $false $null 'clean log' $true 'failed'
    Test-Case 'live-validation-error' $false 0 'DXGI_ERROR_DEVICE_REMOVED injected' $true 'failed'
    [pscustomobject]@{ evidenceType = 'unit-test'; status = 'PASS'; cases = $results.Count
        source = $sourcePath; sessionRecords = 'not accessed'; results = @($results) } | ConvertTo-Json -Depth 5
}
finally {
    # Exact test-owned files only; no recursive deletion and no project records.
    foreach ($file in $ownedFiles) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $testRoot -ErrorAction SilentlyContinue
}
