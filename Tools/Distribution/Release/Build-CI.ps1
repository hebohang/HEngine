param([Parameter(Mandatory=$true)][ValidateSet('x64','arm64')][string]$Architecture)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$work=Join-Path $repo 'out/agent/release-ci'
& python "$PSScriptRoot/contract.py" guard --architecture $Architecture
if ($LASTEXITCODE -ne 0) { throw 'Free-runner policy rejected this run before installation/build.' }
. "$repo/Tools/Agent/Architecture.ps1"
$env:HE_TARGET_ARCHITECTURE=$Architecture
$env:HE_BUILD_PYTHON=(Get-Command python.exe -ErrorAction Stop).Source
$null=New-Item -ItemType Directory -Force "$work/evidence","$work/publish","$work/templates"
# Preserve completed payloads when the standard storage manager retires build caches.
[IO.File]::WriteAllText("$work/.he-keep", 'CI release payload; preserve until this owned job ends.')
$commands=[Collections.Generic.List[object]]::new()
function Stage([string]$Name, [string[]]$Arguments) {
    $log=Join-Path $work "evidence/$Name.log"
    $preference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass @Arguments *> $log
        $code=$LASTEXITCODE
    } finally { $ErrorActionPreference=$preference }
    if ($code -ne 0) { Get-Content -LiteralPath $log -Tail 40; throw "Release stage failed: $Name (exit $code)" }
    $commands.Add([ordered]@{stage=$Name;program='powershell.exe';arguments=$Arguments;exitCode=$code})
    Write-Output "PASS: $Name"
}
function RequireSpace {
    $volume=Get-PSDrive -Name ([IO.Path]::GetPathRoot($repo).TrimEnd('\').TrimEnd(':'))
    if ($volume.Free -lt 4GB) {
        & "$repo/hectl.bat" clean maximize -WhatIf
        if ($LASTEXITCODE -ne 0) { throw 'Storage preview failed; no alternative deletion attempted.' }
        & "$repo/hectl.bat" clean maximize
        if ($LASTEXITCODE -ne 0) { throw 'Storage reclamation failed; no paid runner fallback.' }
        $volume=Get-PSDrive -Name $volume.Name
        if ($volume.Free -lt 4GB) { throw 'Insufficient free standard-runner disk; stop without switching to a paid runner.' }
    }
}
RequireSpace
Stage 'dependencies' @('-File',"$repo/Tools/Dependencies/Install-SourceDependencies.ps1",'-NoPause')
if ($Architecture -eq 'arm64') {
    # Preparation pins official ARM binaries and source-built dependencies.
    Stage 'arm64-dependencies' @('-File',"$repo/Tools/Dependencies/Prepare-WindowsArm64.ps1")
    $env:HE_WINDOWS_ARM64_ROOT="$repo/out/agent/dependencies/windows-arm64"
}
. "$repo/Tools/Agent/Project.ps1"
$project=Resolve-HEProject "$repo/Engine/Templates/Blank" $repo
$version=Get-HEEngineVersion $repo
$sourceCommit=(& git -C $repo rev-parse HEAD).Trim()
if ($sourceCommit -cne $env:GITHUB_SHA) { throw 'Checkout differs from workflow source commit.' }
$platform="Windows-$Architecture"
$windows="$work/templates/$platform"
$development="$work/templates/Development-$platform"
$android="$work/templates/Android-arm64"
RequireSpace
Stage 'editor-build' @('-File',"$repo/Tools/Agent/Build.ps1",'-Target','HEngineEditor','-Configuration','RelWithDebInfo',
    '-Project',$project.Root,'-PortableEditor','-IncludeRenderSmokes','-PluginConfig',"$repo/Tools/Distribution/Plugins.json",'-Architecture',$Architecture,'-Parallel','2')
$mode=if($Architecture -eq 'x64'){'editor-portable'}else{'editor-portable-arm64'}
$editorBuild="$repo/out/agent/projects/$($project.CacheKey)/$mode-relwithdebinfo"
Stage 'fsr-native-dispatch' @('-File',"$PSScriptRoot/Test-FsrRelease.ps1",'-BuildRoot',$editorBuild,
    '-Architecture',$Architecture,'-Evidence',"$work/evidence/fsr")
$editorSnapshot="$work/editor-payload"
foreach ($part in @('bin','GeneratedContent')) {
    & robocopy.exe "$editorBuild/$part" "$editorSnapshot/$part" /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XF *.pdb *.ilk *.lib *.exp | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'Could not preserve the completed Editor payload.' }
}
$parser=if($Architecture -eq 'arm64'){"$env:HE_WINDOWS_ARM64_ROOT/host-parser/HEngineParser.exe"}else{"$editorSnapshot/bin/Programs/HEngineParser/HEngineParser.exe"}
RequireSpace
Stage 'shipping-template' @('-File',"$repo/Tools/Distribution/Build-WindowsTemplate.ps1",'-Output',$windows,
    '-Architecture',$Architecture,'-PackageOutputRoot',"$work/packages")
RequireSpace
Stage 'development-template' @('-File',"$repo/Tools/Distribution/Build-DevPlayer.ps1",'-Output',$development,'-Architecture',$Architecture)
$env:HE_ANDROID_TOOLCHAIN_ROOT=Join-Path $env:RUNNER_TEMP 'he-android'
Stage 'android-tools' @('-File',"$repo/Tools/Android/Setup.ps1",'-AcceptLicenses','-NativeCompiler')
RequireSpace
Stage 'android-template' @('-File',"$repo/Tools/Distribution/Build-AndroidTemplate.ps1",'-Output',$android,
    '-WindowsTemplate',$windows,'-HostParser',$parser)
$release="$work/assembled"
$sdk="$release/HEngine-$version-$platform"
Stage 'sdk-assembly' @('-File',"$repo/Tools/Distribution/Build-Editor.ps1",'-SkipBuild','-Architecture',$Architecture,'-Output',$sdk,
    '-WindowsTemplate',$windows,'-DevelopmentTemplate',$development,'-AndroidTemplate',$android,'-EditorBuildDirectory',$editorSnapshot)
. "$repo/Tools/Android/Toolchain.ps1"
$null=Import-HeAndroidToolchain
Stage 'sdk-export-tools' @('-File',"$sdk/Tools/Dependencies/Dependencies.ps1",'-Action','install','-Profile','SDK','-Project',"$sdk/Engine/Templates/Blank")
& python "$PSScriptRoot/contract.py" audit --root $sdk --architecture $Architecture --output "$work/evidence/architecture.json"
if ($LASTEXITCODE -ne 0) { throw 'SDK includes wrong-architecture binaries.' }
Stage 'sdk-api-gameplay' @('-File',"$sdk/Tools/Distribution/Test-ScriptSdk.ps1",'-Engine',$sdk,'-Evidence',"$work/validation/api",'-UseWarp')
# The SDK's template generator supplies the same current-version game to all exporters.
& "$sdk/hectl.bat" project create -Project "$work/validation/game" -Name ReleaseArena -Template SharedArena
if ($LASTEXITCODE -ne 0) { throw 'Could not create independent validation game.' }
Stage 'sdk-editor-startup' @('-File',"$sdk/Tools/Agent/ProjectLaunch.ps1",'-Project',"$work/validation/game",'-Check',
    '-Log',"$work/evidence/editor-launch.log",'-UseWarp','-StartupTimeoutSeconds','300')
Stage 'sdk-windows-export' @('-File',"$repo/Tools/Distribution/Test-WindowsExporter.ps1",'-Project',"$work/validation/game",'-ValidationRoot',$sdk,'-SkipAssembly')
Stage 'sdk-android-export' @('-File',"$repo/Tools/Distribution/Test-AndroidExporter.ps1",'-Project',"$work/validation/game",'-Engine',$sdk)
foreach($profile in @('web','wechat')) {
    Stage "sdk-$profile-export" @('-File',"$repo/Tools/Distribution/Test-PortableExporter.ps1",'-Project',"$work/validation/game",'-Engine',$sdk,'-Profile',$profile)
}
$result=[ordered]@{schema=1;pass=$true;architecture=$Architecture;version=$version;sourceCommit=$sourceCommit;
    runner=$env:HE_RUNNER_LABEL;passedChecks=@('layout','native-architecture','fsr-native-dispatch','editor-startup','api-gameplay','windows-export','android-export','web-export','wechat-export');
    nativeCapabilities=(Get-Content "$sdk/HEngine.Runtime.json" -Raw | ConvertFrom-Json);
    workflowRun="https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID";commands=@($commands);
    limitations=@('Editor startup is not an interactive Play/Stop test','No physical GPU, Android phone, or WeChat device validation')}
# Preserve full build provenance inside the SDK before hashing the final ZIP.
# Archive hashes remain outside the archive to avoid a self-referential digest.
$null=New-Item -ItemType Directory -Force "$sdk/Docs/Release"
$result | ConvertTo-Json -Depth 6 | Set-Content "$sdk/Docs/Release/RELEASE_PROVENANCE.json" -Encoding UTF8
& python "$repo/Tools/Distribution/Archive-Release.py" --release $release --engine-only --version $version --architecture $Architecture
if ($LASTEXITCODE -ne 0) { throw 'SDK ZIP round-trip failed.' }
foreach($file in @("HEngine-$version-$platform.zip",'artifacts.json','SHA256SUMS.txt')) { Copy-Item -LiteralPath "$release/$file" -Destination "$work/publish" }
$result | ConvertTo-Json -Depth 6 | Set-Content "$work/publish/result.json" -Encoding UTF8
