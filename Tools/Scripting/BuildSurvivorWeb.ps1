param([string]$Python = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$previousPython = $env:HE_BUILD_PYTHON
try {
    if ($Python) { $env:HE_BUILD_PYTHON = $Python }
    & (Join-Path $repo 'Tools/Build/Publish.ps1') -Project (Join-Path $repo 'Project') -Profile web
} finally { $env:HE_BUILD_PYTHON = $previousPython }
