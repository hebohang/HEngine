param([string]$Root = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'LibClang.ps1')
Resolve-HELibClang -Root $Root | ConvertTo-Json -Compress
