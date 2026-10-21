param(
    [string]$Name,

    [switch]$List,

    [string]$Project = "",
    [string]$PluginConfig = "",

    [ValidateSet("Debug", "RelWithDebInfo")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 16)]
    [int]$Parallel = 4,

    [ValidateRange(1, 300)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Process.ps1")

if ($List -and $Name) {
    throw "Use either -List or -Name, not both."
}
if (-not $List -and [string]::IsNullOrWhiteSpace($Name)) {
    throw "Specify exactly one test with -Name, or use -List."
}
if ($Name -and $Name -notmatch "^[A-Za-z0-9_-]+$") {
    throw "Test names may contain only letters, digits, underscores, and hyphens."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "Project.ps1")
$projectInfo = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repositoryRoot
$buildScript = Join-Path $PSScriptRoot "Build.ps1"
& $buildScript -Target HEngineTests -Configuration $Configuration -Parallel $Parallel -Project $projectInfo.Root -PluginConfig $PluginConfig
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$configurationName = $Configuration.ToLowerInvariant()
$bDefaultProject = [string]::Equals($projectInfo.Root.TrimEnd('\','/'), (Join-Path $repositoryRoot 'Project'), [StringComparison]::OrdinalIgnoreCase)
$agentRoot = if ($bDefaultProject) { Join-Path $repositoryRoot 'out/agent' } else { Join-Path $repositoryRoot "out/agent/projects/$($projectInfo.CacheKey)" }
$buildRoot = Join-Path $agentRoot "tests-$configurationName"
$artifact = Join-Path $buildRoot "bin\HEngineTests.exe"
if (-not (Test-Path -LiteralPath $artifact)) {
    throw "HEngineTests artifact was not found: $artifact"
}

$logRoot = Join-Path $agentRoot 'logs'
$testLabel = if ($List) { "list" } else { $Name }
$runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $PID
$stdoutLog = Join-Path $logRoot ("test-{0}-{1}-{2}.stdout.log" -f $configurationName, $testLabel, $runId)
$stderrLog = Join-Path $logRoot ("test-{0}-{1}-{2}.stderr.log" -f $configurationName, $testLabel, $runId)

$arguments = if ($List) { @("--list") } else { @("--run", $Name) }
$executionLockPath = Join-Path $buildRoot '.agent-execution.lock'
$executionLock = $null
$process = $null
$processJob = $null
try {
    try {
        $executionLock = [System.IO.File]::Open(
            $executionLockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
    }
    catch {
        throw "Another Agent build or owned run is using the tests build directory. Wait for it to finish."
    }

    $processJob = New-AgentProcessJob
    $process = Start-Process -FilePath $artifact -ArgumentList $arguments -WorkingDirectory $repositoryRoot -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog -PassThru
    # Windows PowerShell 5.1 can lose ExitCode for very short-lived child processes
    # unless the native process handle is materialized before it exits.
    $null = $process.Handle
    try {
        Add-AgentProcessToJob -Job $processJob -Process $process
    }
    catch {
        # A bounded logic test can finish between Start-Process and Job
        # assignment. Its exit code remains authoritative; live-process
        # assignment failures must still abort and enter the cleanup path.
        $process.Refresh()
        if (-not $process.HasExited) { throw }
    }

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-AgentOwnedProcess -Process $process -Description "HEngineTests"
        Get-Content -LiteralPath $stdoutLog -Tail 50 -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $stderrLog -Tail 50 -ErrorAction SilentlyContinue
        throw "HEngineTests timed out after $TimeoutSeconds seconds: $testLabel"
    }
    $process.WaitForExit()
    $process.Refresh()

    if ($null -eq $process.ExitCode) {
        throw "HEngineTests exited without an observable exit code: $testLabel"
    }

    Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue
        Write-Host "unit-test ${testLabel}: FAIL exit=$($process.ExitCode)"
        exit $process.ExitCode
    }

    Write-Host "unit-test ${testLabel}: PASS"
}
finally {
    try {
        try {
            if ($null -ne $process) {
                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-AgentOwnedProcess -Process $process -Description "HEngineTests"
                }
            }
        }
        finally {
            if ($null -ne $processJob) {
                Close-AgentProcessJob -Job $processJob
            }
        }
    }
    finally {
        if ($null -ne $executionLock) {
            $executionLock.Dispose()
        }
    }
}
