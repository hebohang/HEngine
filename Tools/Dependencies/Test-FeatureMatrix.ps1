param([string]$Root = 'out/agent/dependency-validation/feature-matrix')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Agent/Toolchain.ps1')
Import-HeToolchain
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$cases = [ordered]@{PHYSICS_2D='box2d'; PHYSICS_3D='JoltPhysics'; NAVIGATION='recastnavigation'; UI_SVG='PlutoVG|LunaSVG'; UI_LOTTIE='rlottie'}
$results = @()
foreach ($disabled in $cases.Keys) {
    $build = Join-Path $Root $disabled.ToLowerInvariant()
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $flags = @($cases.Keys | ForEach-Object { "-DHE_ENABLE_$_=" + $(if ($_ -eq $disabled) {'OFF'} else {'ON'}) })
    & cmake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHE_BUILD_DEV_GAME=ON -DPACK_GAME=OFF -DHE_ENABLE_GM=ON -DHE_RENDER_DX12=ON -DHE_RENDER_VULKAN=OFF -DHE_SHADER_SPIRV=OFF @flags *> (Join-Path $build 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw "Feature matrix configure failed: $disabled" }
    $commands = @(& ninja -C $build -t commands HGame)
    if ($LASTEXITCODE -ne 0) { throw "Feature matrix graph failed: $disabled" }
    $sources = @($commands | ForEach-Object { if ($_ -match ' -c (.+)$') { $matches[1].Replace('\','/') } })
    foreach ($feature in $cases.Keys) {
        $present = @($sources | Where-Object { $_ -match "/ThirdParty/($($cases[$feature]))/" }).Count -gt 0
        if ($present -eq ($feature -eq $disabled)) { throw "Incorrect dependency graph: disabled=$disabled feature=$feature present=$present" }
    }
    foreach ($common in @('/Platform/Scripting/PcScriptServices.cpp', '/Platform/RenderBackendFactory.cpp')) {
        if (!@($sources | Where-Object { $_ -like "*$common*" }).Count) { throw "Common platform source lost: $disabled $common" }
    }
    $physics2d = @($sources | Where-Object { $_ -like '*/Physics/PhysicSystem2D.cpp*' }).Count -gt 0
    if ($physics2d -eq ($disabled -eq 'PHYSICS_2D')) { throw "Incorrect physics implementation graph: $disabled" }
    $results += @{disabled=$disabled;compileCommands=$sources.Count;result='PASS';evidence='static-check'}
}
$results | ConvertTo-Json
