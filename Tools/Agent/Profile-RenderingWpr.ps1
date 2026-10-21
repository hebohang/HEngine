[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$StartupScene,
    [Parameter(Mandatory=$true)][string]$BenchmarkInputPath,
    [Parameter(Mandatory=$true)][string]$RenderSettingsPath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('Forward','Deferred')][string]$RenderPath = 'Deferred'
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new trace directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. (Join-Path $PSScriptRoot 'Storage.ps1')
$storageRun = Start-HEStorageRun -Project (Join-Path $repositoryRoot 'Project') -Workflow 'Profile-RenderingWpr' -Paths @($OutputDirectory) -Outer
$storageState = 'failed'
try {

$instance = 'HEngineRender-' + [guid]::NewGuid().ToString('N')
$trace = Join-Path $OutputDirectory 'rendering.etl'
$started = $false
$wprCompleted = $false
function Invoke-Wpr([string[]]$Arguments, [string]$LogPath) {
    # Windows PowerShell treats native stderr as an ErrorRecord, even when WPR
    # only writes a blank banner. Inspect the native exit code instead.
    $ErrorActionPreference = 'Continue'
    & wpr.exe @Arguments *> $LogPath
    return $LASTEXITCODE
}
@{instance=$instance; trace=$trace} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'owner.json') -Encoding UTF8
try {
    $code = Invoke-Wpr @('-start','CPU','-start','GPU','-filemode','-recordtempto',$OutputDirectory,'-instancename',$instance) `
        (Join-Path $OutputDirectory 'wpr-start.log')
    if ($code -ne 0) { throw "WPR could not start the named CPU/GPU trace; see wpr-start.log (exit=$code)." }
    $started = $true
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Benchmark-Rendering.ps1') `
        -StartupScene $StartupScene -BenchmarkInputPath $BenchmarkInputPath -RenderSettingsPath $RenderSettingsPath `
        -RenderPath $RenderPath -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 600 -MeasureFrames 600 `
        -TimeoutSeconds 300 -PowerModeNote 'Instrumented CPU/GPU ETW diagnostic; not a frame-budget acceptance run' `
        -OutputDirectory (Join-Path $OutputDirectory 'benchmark') *> (Join-Path $OutputDirectory 'benchmark.log')
    if ($LASTEXITCODE -ne 0) { throw "Instrumented benchmark failed; see benchmark.log (exit=$LASTEXITCODE)." }
} finally {
    if ($started) {
        $code = Invoke-Wpr @('-stop',$trace,'-instancename',$instance) (Join-Path $OutputDirectory 'wpr-stop.log')
        if ($code -eq 0) { $wprCompleted = $true }
        if ($code -ne 0) {
            # Cancel only this script's named session if saving failed.
            $null = Invoke-Wpr @('-cancel','-instancename',$instance) (Join-Path $OutputDirectory 'wpr-cancel.log')
            throw 'WPR trace save failed; the owned instance was cancelled. See wpr-stop.log.'
        }
    } else {
        $null = Invoke-Wpr @('-cancel','-instancename',$instance) (Join-Path $OutputDirectory 'wpr-cancel.log')
    }
}
@{
    evidenceType='runtime-manual'; trace=$trace; instance=$instance;
    profile='CPU + GPU, file mode'; nativeResolution='3840x2160';
    interpretation='Instrumented timeline including startup and warmup; not uninstrumented performance or thermal acceptance.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'trace.json') -Encoding UTF8
Write-Host "CPU/GPU timeline saved: $trace"

$storageState = 'success'
}
finally {
    if ($started -and -not $wprCompleted) { $storageState = 'unknown' }
    Complete-HEStorageRun -Run $storageRun -State $storageState
}
