param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Profile = 'windows',
    [ValidateSet('Check','Build')][string]$Action = 'Build',
    [string]$OutputDirectory = '',
    [string]$WorkDirectory = '',
    [string]$Template = ''
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/Export-Native.ps1" @PSBoundParameters -Platform Windows