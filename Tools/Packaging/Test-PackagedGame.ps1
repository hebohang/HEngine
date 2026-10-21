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

    [switch]$UseWarp,
    [ValidateLength(0,256)][string]$RequiredLogMarker = '',
    [switch]$MeasureMemory
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
    $exited = $false
    if ($MeasureMemory) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $samples = [Collections.Generic.List[object]]::new()
        do {
            try {
                $process.Refresh()
                $samples.Add([ordered]@{elapsedMs=$timer.Elapsed.TotalMilliseconds;workingSetBytes=$process.WorkingSet64;
                    privateBytes=$process.PrivateMemorySize64;observedOsPeakWorkingSetBytes=$process.PeakWorkingSet64})
            } catch [InvalidOperationException] { } # Process can exit between observation and read.
            $exited = $process.WaitForExit(50)
        } while (!$exited -and $timer.Elapsed.TotalSeconds -lt $TimeoutSeconds)
        @{measurement='Windows process memory sampled every approximately 50ms after Start-Process returned. Includes engine, all script runtimes and active renderer. Sampled maxima/last observed OS peak may miss exit-time allocations; not managed heap size, GPU memory or first-frame timing.';
            package=$packagePath;pid=$process.Id;graphicsApi=$GraphicsApi;warp=$UseWarp.IsPresent;samples=$samples} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logRoot ($runId + '.memory.json'))
    } else { $exited = $process.WaitForExit($TimeoutSeconds * 1000) }
    if (-not $exited) {
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
    if ($RequiredLogMarker -and !(Select-String -LiteralPath $stdout -Pattern $RequiredLogMarker -SimpleMatch -Quiet)) {
        Get-Content -LiteralPath $stdout -Tail 40 -ErrorAction SilentlyContinue
        throw "Packaged gameplay did not reach its required marker: $RequiredLogMarker"
    }
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
    # Validate this package's reachable content, including Python modules when
    # present, rather than requiring one particular game's UI implementation.
    $manifest = Get-Content -LiteralPath (Join-Path $packagePath 'Game.manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($entry in $manifest.Files) {
        $extractedFile = Join-Path $freshContent $entry.Path
        $fileIoPath = '\\?\' + $extractedFile
        if (-not [IO.File]::Exists($fileIoPath) -or
            ([IO.FileInfo]::new($fileIoPath)).Length -ne $entry.StoredSize) {
            throw "Freshly extracted package content is missing or has the wrong size: $($entry.Path)"
        }
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
        # Imported content can have deeply nested hashed filenames. PowerShell's
        # ordinary path spelling fails to traverse those entries past MAX_PATH.
        Remove-Item -LiteralPath ('\\?\' + $resolvedCheckRoot) -Recurse -Force
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
