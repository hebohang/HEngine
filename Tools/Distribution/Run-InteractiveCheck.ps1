param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [string]$Project = '',
    [string]$Scene = '',
    [switch]$Packaged,
    [ValidateRange(30,900)][int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$repo/Tools/Agent/Process.ps1"
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$evidencePath = [IO.Path]::GetFullPath($Evidence)
$null = New-Item -ItemType Directory -Force -Path $evidencePath
$process = $null
try {
    $arguments = @()
    if ($Packaged) { $arguments += @('--package-check', ('--package-check-id=' + [guid]::NewGuid().ToString('N'))) }
    $arguments += @('--resolution=1280x720', '--no-cursor-capture', '--exit-after-frames=1000000')
    if ($Project) { $arguments += '--project="' + (Resolve-Path -LiteralPath $Project).Path + '"' }
    if ($Scene) { $arguments += '--startup-scene=' + $Scene }
    $process = Start-Process -FilePath $executablePath -ArgumentList $arguments -WorkingDirectory (Split-Path $executablePath) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput "$evidencePath/stdout.log" -RedirectStandardError "$evidencePath/stderr.log"
    # Pin the process handle before it exits so Windows PowerShell retains ExitCode.
    $null = $process.Handle
    [ordered]@{pid=$process.Id;executable=$executablePath;arguments=$arguments;sha256=(Get-FileHash $executablePath).Hash} |
        ConvertTo-Json | Set-Content -LiteralPath "$evidencePath/owned-process.json" -Encoding UTF8
    Write-Output "Owned interactive check: pid=$($process.Id); evidence=$evidencePath"
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) { throw 'Interactive check reached its time limit.' }
    $process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $evidencePath 'exit-code.txt'), [string]$process.ExitCode)
    if ($process.ExitCode -ne 0) { throw "Application exited with code $($process.ExitCode)." }
    Write-Output 'Application closed normally. Gameplay evidence must be reviewed separately.'
}
finally {
    if ($process) {
        if (!$process.HasExited) { Stop-AgentOwnedProcess -Process $process -Description 'release interactive check' }
        $process.Dispose()
    }
}
