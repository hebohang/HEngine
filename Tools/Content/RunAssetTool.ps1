param([Parameter(Mandatory=$true)][string]$Engine,
      [Parameter(Mandatory=$true)][string]$Project,
      [Parameter(Mandatory=$true)][string]$Source,
      [Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference='Stop'
. (Join-Path $Engine 'Tools/Dependencies/Environment.ps1')
Import-HEDependencyEnvironment
if (!$env:HE_BUILD_PYTHON -or !(Test-Path -LiteralPath $env:HE_BUILD_PYTHON)) {
    throw 'Python 3.11 is unavailable. Run hectl deps install -Profile SDK.'
}
& $env:HE_BUILD_PYTHON -X utf8 (Join-Path $Engine 'Tools/Content/asset_operations.py') move --project $Project --source $Source --target $Target
exit $LASTEXITCODE
