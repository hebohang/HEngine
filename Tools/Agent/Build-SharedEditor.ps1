param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [string]$Target = "HEngineEditor"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildRootPath = if ([System.IO.Path]::IsPathRooted($BuildRoot)) {
    $BuildRoot
}
else {
    Join-Path $repositoryRoot $BuildRoot
}
$cachePath = Join-Path $buildRootPath "CMakeCache.txt"
$lockPath = Join-Path $buildRootPath ".ninja_lock"

if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "The selected Visual Studio build directory has no CMake cache: $buildRootPath"
}

$generatorMatch = Select-String -Path $cachePath -Pattern "^CMAKE_GENERATOR:" | Select-Object -First 1
$bNinja = $generatorMatch -and $generatorMatch.Line.EndsWith("=Ninja")
if ($bNinja -and (Test-Path -LiteralPath $lockPath)) {
    $processes = @(Get-Process | Where-Object {
        $_.ProcessName -match "devenv|MSBuild|ninja|cmake|clang|lld|llvm"
    } | Sort-Object ProcessName, Id)
    $processSummary = if ($processes.Count -eq 0) {
        "none"
    }
    else {
        ($processes | ForEach-Object { "{0}#{1}" -f $_.ProcessName, $_.Id }) -join ", "
    }
    throw "Shared build root is busy: $lockPath. Related processes: $processSummary"
}

Set-Location $repositoryRoot
Write-Host "shared VS build: cmake --build $BuildRoot --target $Target"
& cmake --build $buildRootPath --target $Target
exit $LASTEXITCODE
