param(
    [string]$Output = '',
    [Parameter(Mandatory=$true)][string]$WindowsTemplate,
    [Parameter(Mandatory=$true)][string]$HostParser,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$PSScriptRoot/Template.ps1"
. "$repo/Tools/Android/Toolchain.ps1"
. "$repo/Tools/Dependencies/Environment.ps1"
Import-HEDependencyEnvironment
$android = Import-HeAndroidToolchain
$hostPlatform = (Get-Content -LiteralPath "$WindowsTemplate/Template.json" -Raw | ConvertFrom-Json).Platform
if ($hostPlatform -notin @('Windows-x64','Windows-arm64')) { throw 'Android template requires Windows host tools.' }
$hostTemplate = Get-HEExportTemplate $repo $hostPlatform $WindowsTemplate
$parser = (Resolve-Path -LiteralPath $HostParser).Path
if (!$Output) { $Output = "$repo/out/agent/export-templates/Android-arm64" }
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh Android template output directory.' }
$build = Join-Path $repo 'out/agent/android-player-release'
$ndk = $env:ANDROID_NDK_HOME
$ndkToolchain = Join-Path $ndk 'build/cmake/android.toolchain.cmake'
if (!(Test-Path -LiteralPath $ndkToolchain)) { throw 'Install the locked Android NDK before producing export templates.' }
if ((Get-Content -Raw -LiteralPath "$ndk/source.properties") -notmatch [regex]::Escape("Pkg.Revision = $($android.Pin.ndk.version)")) {
    throw 'The template producer requires the locked NDK version.'
}
. "$repo/Tools/Agent/Toolchain.ps1"
Import-HeToolchain
function Run-Native([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Android template tool failed: $Tool ($LASTEXITCODE)" }
}
if (!$SkipBuild) {
    Run-Native 'cmake' @('-S',$repo,'-B',$build,'-G','Ninja',"-DCMAKE_TOOLCHAIN_FILE=$ndkToolchain",
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-29','-DCMAKE_BUILD_TYPE=Release',
        "-DHE_PROJECT_DIR=$repo/Engine/Templates/Blank","-DHE_PLUGIN_CONFIG=$PSScriptRoot/PlayerPlugins.json",
        '-DHE_PORTABLE_PLAYER=ON',"-DHE_CODEGEN_PARSER_EXECUTABLE=$parser")
    Run-Native 'cmake' @('--build',$build,'--target','HGame','-j','2')
}
$contract = Get-Content -Raw -LiteralPath "$build/PlayerTemplate-Release.json" | ConvertFrom-Json
if ($contract.Platform -cne 'Android-arm64' -or !$contract.PackGame -or $contract.DevAgent -or
    $contract.Configuration -ne 'Release' -or $contract.ScriptApiHash -cne $hostTemplate.Manifest.ScriptApiHash -or
    (@($contract.Plugins) -join ',') -cne (@($hostTemplate.Manifest.Plugins) -join ',')) {
    throw 'Android and host tool templates do not have matching Shipping script/plugin contracts.'
}
$cache = Get-Content -Raw -LiteralPath "$build/CMakeCache.txt"
foreach ($expected in @('HE_PORTABLE_PLAYER:BOOL=ON','PACK_GAME:BOOL=ON','HE_BUILD_DEV_GAME:BOOL=OFF','HE_ENABLE_GM:BOOL=OFF')) {
    if ($cache -notmatch "(?m)^$([regex]::Escape($expected))\r?$") { throw "Invalid template build flag: $expected" }
}
$stage = $destination + '.staging-' + [guid]::NewGuid().ToString('N')
$null = New-Item -ItemType Directory -Path "$stage/Player","$stage/Policy" -Force
Copy-Item -LiteralPath "$build/bin/libhegame.so" -Destination "$stage/Player/libhegame.so"
Run-Native "$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" @('--strip-unneeded',"$stage/Player/libhegame.so")
$python = if ($env:HE_BUILD_PYTHON) { $env:HE_BUILD_PYTHON } else { (Get-Command python.exe).Source }
Run-Native $python @("$repo/Tools/Android/VerifyPackage.py",'--elf',"$stage/Player/libhegame.so")
Run-Native 'cmake' @("--graphviz=$build/PackageTargetGraph.dot",$build)
Copy-Item -LiteralPath "$build/PackageTargetGraph.dot" -Destination "$stage/Policy/PackageTargetGraph.dot"
& "$repo/Tools/Packaging/Verify-PackagePolicy.ps1" -TargetGraphPath "$stage/Policy/PackageTargetGraph.dot" -RequireBuildGraphInput
foreach ($relative in $hostTemplate.Files.Keys) {
    if ($relative.StartsWith('Tools/') -or $relative.StartsWith('GeneratedContent/') -or
        $relative.StartsWith('Player/Licenses/') -or $relative -ceq 'Player/LICENSE.txt') {
        $target = Join-Path $stage $relative
        $null = New-Item -ItemType Directory -Path (Split-Path $target) -Force
        Copy-Item -LiteralPath $hostTemplate.Files[$relative] -Destination $target
    }
}
foreach ($pair in @{
    Player='Player/libhegame.so';Cooker=$hostTemplate.Manifest.Cooker;ShaderCompiler=$hostTemplate.Manifest.ShaderCompiler
    MinimumSdk=29;NdkVersion=$android.Pin.ndk.version
}.GetEnumerator()) { $contract | Add-Member -MemberType NoteProperty -Name $pair.Key -Value $pair.Value -Force }
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{Path=$_.FullName.Substring($stage.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
$contract | Add-Member -MemberType NoteProperty -Name Files -Value $files
[IO.File]::WriteAllText("$stage/Template.json",($contract | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
$null = Get-HEExportTemplate $repo 'Android-arm64' $stage
$null = New-Item -ItemType Directory -Path (Split-Path $destination) -Force
[IO.Directory]::Move($stage,$destination)
Write-Output "Android export template: $destination"
