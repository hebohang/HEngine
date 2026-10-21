# Shared by hectl, standalone codegen, packaging, and CI. Dot-source only.
. (Join-Path $PSScriptRoot '../Dependencies/Environment.ps1')
. (Join-Path $PSScriptRoot 'Architecture.ps1')
Import-HEDependencyEnvironment
function Get-HeVisualStudio {
    param([string]$Architecture = '')
    $Architecture = Resolve-HeWindowsArchitecture $Architecture
    $hostArchitecture = Get-HeWindowsHostArchitecture
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe was not found; install the Visual Studio 2026 C++ toolchain."
    }
    $component = if ($Architecture -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
    $installations = @(& $vswhere -latest -version "[17.0,19.0)" -products * `
        -requires $component -format json | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $installations.Count -eq 0) {
        throw "Visual Studio 2026 or 2022 with the $Architecture C++ toolchain was not found."
    }
    $installation = $installations[0]
    $major = ([version]$installation.installationVersion).Major
    $toolsVersion = (Get-Content -LiteralPath (Join-Path $installation.installationPath `
        "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt") -Raw).Trim()
    [pscustomobject]@{
        Path = $installation.installationPath
        Version = $installation.installationVersion
        Generator = if ($major -eq 18) { "Visual Studio 18 2026" } else { "Visual Studio 17 2022" }
        Toolset = if ($major -eq 18) { "v145" } else { "v143" }
        ToolsVersion = $toolsVersion
        Architecture = $Architecture
        HostArchitecture = $hostArchitecture
        Compiler = (Join-Path $installation.installationPath "VC\Tools\MSVC\$toolsVersion\bin\Host$hostArchitecture\$Architecture\cl.exe").Replace('\', '/')
    }
}

function Import-HeToolchain {
    param($Toolchain = (Get-HeVisualStudio))

    $env:VSLANG = "1033"
    $devCommand = Join-Path $Toolchain.Path "Common7\Tools\VsDevCmd.bat"
    # Reinitialize even when invoked from another VS version's developer prompt.
    $environmentLines = & $env:ComSpec /d /s /c `
        "set VSCMD_VER=&& `"$devCommand`" -no_logo -arch=$($Toolchain.Architecture) -host_arch=$($Toolchain.HostArchitecture) >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed to initialize the $($Toolchain.Architecture) toolchain."
    }
    foreach ($line in $environmentLines) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    # Windows PowerShell process launching rejects duplicate PATH/Path entries.
    $processPath = $env:Path
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    Remove-Item Env:Path -ErrorAction SilentlyContinue
    $cmakeRoot = Join-Path $Toolchain.Path "Common7\IDE\CommonExtensions\Microsoft\CMake"
    $env:PATH = "$(Join-Path $cmakeRoot 'CMake\bin');$(Join-Path $cmakeRoot 'Ninja');$processPath"
    Import-HEDependencyEnvironment
    $env:HE_TARGET_ARCHITECTURE = $Toolchain.Architecture
    if (-not (Test-Path -LiteralPath $Toolchain.Compiler -PathType Leaf)) {
        throw "Selected $($Toolchain.Architecture) compiler was not found: $($Toolchain.Compiler)"
    }
    $cmakeVersionOutput = @(& cmake --version)
    $cmakeExitCode = $LASTEXITCODE
    $cmakeVersionLine = $cmakeVersionOutput | Select-Object -First 1
    $cmakeVersionMatch = [regex]::Match($cmakeVersionLine, 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)')
    $minimumCmake = if ($Toolchain.Toolset -eq "v145") { [version]'4.2' } else { [version]'3.24' }
    if ($cmakeExitCode -ne 0 -or -not $cmakeVersionMatch.Success -or
        [version]$cmakeVersionMatch.Groups[1].Value -lt $minimumCmake) {
        throw "Selected toolchain requires CMake >= $minimumCmake; found '$cmakeVersionLine'."
    }
}

function Get-HeFreshConfigureArguments {
    param([string]$BuildRoot, [string]$Compiler, [string]$Generator)

    $cachePath = Join-Path $BuildRoot "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) { return }
    $cache = Get-Content -LiteralPath $cachePath
    $compilerLine = $cache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:[^=]+=' } | Select-Object -First 1
    $generatorLine = $cache | Where-Object { $_ -match '^CMAKE_GENERATOR:[^=]+=' } | Select-Object -First 1
    $cachedCompiler = if ($compilerLine) { ($compilerLine -split '=', 2)[1].Replace('\', '/') } else { "" }
    $cachedGenerator = if ($generatorLine) { ($generatorLine -split '=', 2)[1] } else { "" }
    # Visual Studio generators record the compiler in CMakeFiles rather than
    # CMakeCache.txt. Read that metadata without reconfiguring a clean cache.
    if (-not $cachedCompiler) {
        $compilerFiles = @(Get-ChildItem -Path (Join-Path $BuildRoot 'CMakeFiles/*/CMakeCXXCompiler.cmake') `
            -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
        if ($compilerFiles.Count -gt 0) {
            $compilerLine = Get-Content -LiteralPath $compilerFiles[0].FullName |
                Where-Object { $_ -match '^set\(CMAKE_CXX_COMPILER "([^"]+)"\)' } | Select-Object -First 1
            if ($compilerLine -match '^set\(CMAKE_CXX_COMPILER "([^"]+)"\)') {
                $cachedCompiler = $matches[1].Replace('\', '/')
            }
        }
    }
    if ($cachedCompiler -ine $Compiler.Replace('\', '/') -or $cachedGenerator -ne $Generator) {
        # Preserve artifacts and generated sources; CMake recreates configuration
        # metadata and the changed compiler commands invalidate object files.
        "--fresh"
    }
}
