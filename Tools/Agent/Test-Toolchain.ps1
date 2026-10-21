$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Toolchain.ps1')
$selectedToolchain = Get-HeVisualStudio
Import-HeToolchain -Toolchain $selectedToolchain
Import-HeToolchain -Toolchain $selectedToolchain
$activeCompiler = (Get-Command cl.exe -CommandType Application).Source.Replace('\', '/')
if ($activeCompiler -ine $selectedToolchain.Compiler -or $env:VCToolsVersion.TrimEnd('\', '/') -ne $selectedToolchain.ToolsVersion) {
    throw 'Repeated environment initialization must select the requested compiler and STL toolset.'
}
Write-Host "unit-test toolchain environment: PASS ($($selectedToolchain.Generator), $($selectedToolchain.ToolsVersion))"
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('he-toolchain-' + [Guid]::NewGuid().ToString('N'))
$cachePath = Join-Path $testRoot 'CMakeCache.txt'
$metadataPath = $null
$metadataRoot = $null
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $compiler = 'C:/VS2026/VC/Tools/MSVC/14.51/bin/Hostx64/x64/cl.exe'
    if (@(Get-HeFreshConfigureArguments $testRoot $compiler Ninja).Count -ne 0) {
        throw 'A new build must not require a cache reset.'
    }
    @('CMAKE_GENERATOR:INTERNAL=Ninja', "CMAKE_CXX_COMPILER:FILEPATH=$compiler") |
        Set-Content -LiteralPath $cachePath
    if (@(Get-HeFreshConfigureArguments $testRoot $compiler.ToUpperInvariant().Replace('/', '\') Ninja).Count -ne 0) {
        throw 'An unchanged compiler must preserve incremental builds across path spelling differences.'
    }
    if ((Get-HeFreshConfigureArguments $testRoot 'C:/VS2022/cl.exe' Ninja) -ne '--fresh') {
        throw 'A compiler change must reset configuration metadata.'
    }
    if ((Get-HeFreshConfigureArguments $testRoot $compiler 'Visual Studio 18 2026') -ne '--fresh') {
        throw 'A generator change must reset configuration metadata.'
    }
    $metadataRoot = Join-Path $testRoot 'CMakeFiles/4.3.1'
    New-Item -ItemType Directory -Path $metadataRoot -Force | Out-Null
    $metadataPath = Join-Path $metadataRoot 'CMakeCXXCompiler.cmake'
    ('set(CMAKE_CXX_COMPILER "{0}")' -f $compiler) | Set-Content -LiteralPath $metadataPath
    'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026' | Set-Content -LiteralPath $cachePath
    if (@(Get-HeFreshConfigureArguments $testRoot $compiler 'Visual Studio 18 2026').Count -ne 0) {
        throw 'An unchanged Visual Studio build must preserve incremental builds.'
    }
    Write-Host 'unit-test toolchain cache migration: PASS (5 cases)'
}
finally {
    Remove-Item -LiteralPath $cachePath -ErrorAction SilentlyContinue
    if ($metadataPath) { Remove-Item -LiteralPath $metadataPath -ErrorAction SilentlyContinue }
    if ($metadataRoot) {
        Remove-Item -LiteralPath $metadataRoot -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Split-Path -Parent $metadataRoot) -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $testRoot
}
