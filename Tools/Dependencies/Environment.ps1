# Process-local paths; no registry/PATH mutation and no installation during build.
function Import-HEDependencyEnvironment {
    if (($PSVersionTable.PSEdition -eq 'Core') -and $IsMacOS) {
        $dependencyRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../out/agent/dependencies/macos'))
        $settingsFile = Join-Path $dependencyRoot 'settings.json'
        $settings = if (Test-Path -LiteralPath $settingsFile) { Get-Content -Raw -LiteralPath $settingsFile | ConvertFrom-Json } else { $null }
        foreach ($relative in @('bin','cmake/bin','ninja','dotnet')) {
            $directory = Join-Path $dependencyRoot $relative
            if ((Test-Path -LiteralPath $directory -PathType Container) -and !($env:PATH.Split(':') -contains $directory)) {
                $env:PATH = "${directory}:$env:PATH"
            }
        }
        if (Test-Path -LiteralPath (Join-Path $dependencyRoot 'dotnet/dotnet')) { $env:DOTNET_ROOT = Join-Path $dependencyRoot 'dotnet' }
        if ($settings -and $settings.dotnetRoot) {
            $env:DOTNET_ROOT = $settings.dotnetRoot
            if (!($env:PATH.Split(':') -contains $settings.dotnetRoot)) { $env:PATH = "$($settings.dotnetRoot):$env:PATH" }
        }
        if (!$env:HE_BUILD_PYTHON) {
            $env:HE_BUILD_PYTHON = if ($settings -and $settings.python) { $settings.python } else { (Get-Command python3 -ErrorAction Stop).Source }
        }
        return
    }
    $dependencyRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../out/agent/dependencies/windows'))
    $pythonPackages = Join-Path $dependencyRoot 'python-packages'
    if ((Test-Path -LiteralPath "$pythonPackages/PIL") -and !($env:PYTHONPATH -split ';' -contains $pythonPackages)) {
        $env:PYTHONPATH = if ($env:PYTHONPATH) { "$pythonPackages;$env:PYTHONPATH" } else { $pythonPackages }
    }
    foreach ($name in @('dotnet','node')) {
        $directory = Join-Path $dependencyRoot $name
        if ((Test-Path -LiteralPath (Join-Path $directory "$name.exe")) -and !($env:Path.Split(';') -contains $directory)) { $env:Path = "$directory;$env:Path" }
    }
    $cmakeDirectory = Join-Path $dependencyRoot 'cmake/bin'
    if ((Test-Path -LiteralPath "$cmakeDirectory/cmake.exe") -and !($env:Path.Split(';') -contains $cmakeDirectory)) {
        $env:Path = "$cmakeDirectory;$env:Path"
    }
    if (Test-Path -LiteralPath (Join-Path $dependencyRoot 'dotnet/dotnet.exe')) {
        $env:DOTNET_ROOT = Join-Path $dependencyRoot 'dotnet'
    }
    if (!$env:HE_BUILD_PYTHON -and (Get-Command py.exe -ErrorAction SilentlyContinue)) {
        $pythonPath = & py -3.11 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0) { $env:HE_BUILD_PYTHON = [string]$pythonPath }
    }
}
