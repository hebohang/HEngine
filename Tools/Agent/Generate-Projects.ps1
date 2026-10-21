param([switch]$EnableTracy, [string]$Project = '')

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Toolchain.ps1")
$toolchain = Get-HeVisualStudio
Import-HeToolchain -Toolchain $toolchain
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot 'Project.ps1')
$projectInfo = Resolve-HEProject $Project $repositoryRoot
$buildRoot = Join-Path $repositoryRoot $(if ($EnableTracy) { "build_tracy" } else { "build" })
if ($projectInfo.Root -ine (Join-Path $repositoryRoot 'Project')) {
    $buildRoot = Join-Path $repositoryRoot ("out/agent/projects/{0}/vs{1}" -f $projectInfo.CacheKey, $(if ($EnableTracy) { '-tracy' } else { '' }))
}
$tracy = if ($EnableTracy) { "ON" } else { "OFF" }
$freshConfigure = @(Get-HeFreshConfigureArguments -BuildRoot $buildRoot -Compiler $toolchain.Compiler -Generator $toolchain.Generator)
Write-Host "toolchain: $($toolchain.Generator), $($toolchain.Toolset), MSVC $($toolchain.ToolsVersion)"
& cmake @freshConfigure -S $repositoryRoot -B $buildRoot `
    -DPACK_GAME=OFF -DHE_BUILD_DEV_GAME=OFF -DHE_ENABLE_GM=ON -DCPP_CODE_GEN=OFF `
    "-DHE_ENABLE_TRACY=$tracy" -DHE_TRACY_ON_DEMAND=ON `
    "-DHE_PROJECT_DIR=$($projectInfo.Root)" `
    "-DCMAKE_GENERATOR_INSTANCE=$($toolchain.Path)" `
    -G $toolchain.Generator -A x64 -T $toolchain.Toolset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($EnableTracy) {
    & cmake --build $buildRoot --config Debug --target HEngineEditor
}
else {
    & cmake --build $buildRoot --config Debug
}
exit $LASTEXITCODE
