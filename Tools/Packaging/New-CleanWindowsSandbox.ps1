param(
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [string]$ExecutableName = 'StarbloomSurvivor'
)
$ErrorActionPreference = 'Stop'
if ($ExecutableName -notmatch '^[A-Za-z0-9._-]+$') { throw 'Invalid executable name.' }
$package = (Resolve-Path -LiteralPath $PackageDir).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/Test-CleanWindowsGuest.ps1" -Destination $evidence
$escapedPackage = [Security.SecurityElement]::Escape($package)
$escapedEvidence = [Security.SecurityElement]::Escape($evidence)
@"
<Configuration>
  <Networking>Disable</Networking>
  <VGpu>Enable</VGpu>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$escapedPackage</HostFolder><SandboxFolder>C:\PackageInput</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$escapedEvidence</HostFolder><SandboxFolder>C:\Evidence</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Evidence\Test-CleanWindowsGuest.ps1 -PackageDir C:\PackageInput -EvidenceDirectory C:\Evidence -ExecutableName $ExecutableName -UseWarp</Command></LogonCommand>
</Configuration>
"@ | Set-Content (Join-Path $evidence 'Workflow.wsb') -Encoding UTF8
[ordered]@{schema=1;status='NOT RUN';reason='Open Workflow.wsb in Windows Sandbox or run Test-CleanWindowsGuest.ps1 in an offline clean Windows VM.'} |
    ConvertTo-Json | Set-Content (Join-Path $evidence 'result.json') -Encoding UTF8
Write-Output (Join-Path $evidence 'Workflow.wsb')
