param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$ExecutableName,

    [string]$LogDirectory = "",

    [ValidateRange(10, 900)]
    [int]$TimeoutSeconds = 180,

    [ValidateSet('DX12', 'Vulkan')]
    [string]$GraphicsApi = 'DX12',

    [switch]$UseWarp
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$packagePath = [IO.Path]::GetFullPath($PackageDir).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $packagePath -PathType Container)) {
    throw "Package directory does not exist: $packagePath"
}
$game = Join-Path $packagePath ($ExecutableName + '.exe')
if (-not (Test-Path -LiteralPath $game -PathType Leaf)) {
    throw "Packaged game executable is missing: $game"
}
if ($UseWarp -and $GraphicsApi -ne 'DX12') {
    throw '-UseWarp is valid only with -GraphicsApi DX12.'
}

if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    $LogDirectory = Join-Path ([IO.Path]::GetTempPath()) 'HEnginePackageChecks'
}
$logRoot = [IO.Path]::GetFullPath($LogDirectory)
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$packageCheckId = [Guid]::NewGuid().ToString('N')
$unicodeSegment = 'Unicode-' + [char]0x96EA + '-' +
    [char]::ConvertFromUtf32(0x1F680) + ' Path'
$packageCheckParent = [IO.Path]::GetFullPath(
    (Join-Path (Join-Path ([IO.Path]::GetTempPath()) 'HEnginePackageChecks') $unicodeSegment)).TrimEnd('\', '/')
$packageCheckRoot = Join-Path $packageCheckParent $packageCheckId
if (Test-Path -LiteralPath $packageCheckRoot) {
    throw "Fresh package check root already exists: $packageCheckRoot"
}

$runId = "{0}-{1}-{2}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $PID, $packageCheckId
$stdout = Join-Path $logRoot ("{0}.stdout.log" -f $runId)
$stderr = Join-Path $logRoot ("{0}.stderr.log" -f $runId)

$arguments = [Collections.Generic.List[string]]::new()
$arguments.Add('--package-check')
$arguments.Add("--package-check-id=$packageCheckId")
$arguments.Add("--graphics-api=$($GraphicsApi.ToLowerInvariant())")
$arguments.Add('--strict-graphics-api')
if ($UseWarp) {
    $arguments.Add('--warp')
}

$previousPath = $env:PATH
$previousPythonHome = $env:PYTHONHOME
$previousPythonPath = $env:PYTHONPATH
$process = $null
try {
    # The package must find only its app-local binaries plus Windows components.
    # Some CI/agent hosts expose both PATH and Path; Start-Process rejects that
    # case-insensitive duplicate. Rebuild one intentionally minimal key.
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    Remove-Item Env:Path -ErrorAction SilentlyContinue
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Remove-Item Env:\PYTHONHOME -ErrorAction SilentlyContinue
    Remove-Item Env:\PYTHONPATH -ErrorAction SilentlyContinue

    $process = Start-Process `
        -FilePath $game `
        -ArgumentList $arguments.ToArray() `
        -WorkingDirectory ([IO.Path]::GetTempPath()) `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
        Get-Content -LiteralPath $stdout -Tail 100 -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $stderr -Tail 100 -ErrorAction SilentlyContinue
        throw "Packaged game check timed out after $TimeoutSeconds seconds."
    }
    $process.WaitForExit()
    $process.Refresh()
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stdout -Tail 100 -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $stderr -Tail 100 -ErrorAction SilentlyContinue
        throw "Packaged game check failed with exit code $($process.ExitCode)."
    }

    if (-not (Select-String -LiteralPath $stdout -Pattern '[Game] Ready' -SimpleMatch -Quiet)) {
        Get-Content -LiteralPath $stdout -Tail 100 -ErrorAction SilentlyContinue
        throw 'Packaged game exited without reaching the [Game] Ready marker.'
    }
    $backendMarker = "[Graphics] Active backend: $GraphicsApi"
    if (-not (Select-String -LiteralPath $stdout -Pattern $backendMarker -SimpleMatch -Quiet)) {
        Get-Content -LiteralPath $stdout -Tail 100 -ErrorAction SilentlyContinue
        throw "Packaged game did not confirm the requested renderer: $backendMarker"
    }
    if (-not (Select-String -LiteralPath $stdout -Pattern `
            '[Automation] Completed frame limit: 2' -SimpleMatch -Quiet)) {
        Get-Content -LiteralPath $stdout -Tail 100 -ErrorAction SilentlyContinue
        throw 'Packaged game did not complete its bounded gameplay frames.'
    }
    if ((Get-Item -LiteralPath $stderr).Length -ne 0) {
        Get-Content -LiteralPath $stderr -Tail 100 -ErrorAction SilentlyContinue
        throw 'Packaged game wrote unexpected stderr diagnostics.'
    }

    if (-not (Test-Path -LiteralPath $packageCheckRoot -PathType Container)) {
        throw "Packaged game did not create its isolated check root: $packageCheckRoot"
    }
    $contentDirectories = @(Get-ChildItem -LiteralPath $packageCheckRoot -Force -Directory |
        Where-Object { $_.Name -like 'Content-v*' })
    if ($contentDirectories.Count -ne 1) {
        throw "Packaged game check expected exactly one freshly extracted Content-v* directory; found $($contentDirectories.Count)."
    }
    $freshContent = $contentDirectories[0].FullName
    $freshMarker = Join-Path $freshContent '.package-marker'
    $freshPythonModule = Join-Path $freshContent 'Scripts\Lib\encodings\utf_8.py'
    $freshDiagnosticLog = Join-Path $packageCheckRoot 'Saved\Logs\HEngine.log'
    $forbiddenDevOnlyPaths = @(
        (Join-Path $freshContent 'Assets\DevOnly'),
        (Join-Path $freshContent 'Config\DevOnly'),
        (Join-Path $freshContent 'Scripts\DevOnly'),
        (Join-Path $freshContent 'Shaders\DevOnly')
    )
    if (-not (Test-Path -LiteralPath $freshMarker -PathType Leaf)) {
        throw "Freshly extracted package content is missing its marker: $freshMarker"
    }
    if (-not (Test-Path -LiteralPath $freshPythonModule -PathType Leaf)) {
        throw "Freshly extracted package content is missing an embedded Python module: $freshPythonModule"
    }
    if (-not (Test-Path -LiteralPath $freshDiagnosticLog -PathType Leaf)) {
        throw "Packaged game did not use its isolated Unicode Saved path: $freshDiagnosticLog"
    }
    foreach ($forbiddenPath in $forbiddenDevOnlyPaths) {
        if (Test-Path -LiteralPath $forbiddenPath) {
            throw "Packaged content retained a forbidden developer-only path: $forbiddenPath"
        }
    }
    $policyVerifier = Join-Path $PSScriptRoot 'Verify-PackagePolicy.ps1'
    if (-not (Test-Path -LiteralPath $policyVerifier -PathType Leaf)) {
        throw "Package policy verifier is missing: $policyVerifier"
    }
    & $policyVerifier -ExtractedRoots @($freshContent)

    Write-Host "Packaged game full-start check PASS (graphics=$GraphicsApi, warp=$($UseWarp.IsPresent), exit=0)."
    Write-Host "fresh content: $freshContent"
    Write-Host "stdout: $stdout"
    Write-Host "stderr: $stderr"
}
finally {
    if ($null -ne $process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit()
        }
    }
    if (Test-Path -LiteralPath $packageCheckRoot) {
        $resolvedCheckRoot = [IO.Path]::GetFullPath($packageCheckRoot).TrimEnd('\', '/')
        $resolvedCheckParent = [IO.Path]::GetFullPath(
            (Split-Path -Parent $resolvedCheckRoot)).TrimEnd('\', '/')
        if ($resolvedCheckParent -cne $packageCheckParent -or
            (Split-Path -Leaf $resolvedCheckRoot) -cne $packageCheckId) {
            throw "Refusing to remove an unexpected package check root: $resolvedCheckRoot"
        }
        Remove-Item -LiteralPath $resolvedCheckRoot -Recurse -Force
    }
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    Remove-Item Env:Path -ErrorAction SilentlyContinue
    $env:PATH = $previousPath
    if ($null -eq $previousPythonHome) {
        Remove-Item Env:\PYTHONHOME -ErrorAction SilentlyContinue
    }
    else {
        $env:PYTHONHOME = $previousPythonHome
    }
    if ($null -eq $previousPythonPath) {
        Remove-Item Env:\PYTHONPATH -ErrorAction SilentlyContinue
    }
    else {
        $env:PYTHONPATH = $previousPythonPath
    }
}
