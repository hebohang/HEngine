param(
    [ValidateSet('setup','probe-pc','probe-web','probe-wechat','matrix-pc','build-web','serve-web','build-wechat','preview-wechat')]
    [string]$Action = 'probe-pc'
)
$ErrorActionPreference = 'Stop'
if ($Action -eq 'preview-wechat') {
    & (Join-Path $PSScriptRoot 'PrepareWeChatPreview.ps1')
    return
}
if ($Action -eq 'build-web') {
    & (Join-Path $PSScriptRoot 'BuildWebGame.ps1')
    return
}
if ($Action -eq 'matrix-pc') {
    & (Join-Path $PSScriptRoot 'TestPluginMatrix.ps1')
    return
}
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$work = Join-Path $repository 'out/agent/scripting'
$probe = Join-Path $PSScriptRoot 'Probe'
New-Item -ItemType Directory -Force $work | Out-Null
. (Join-Path $repository 'Tools/Agent/Toolchain.ps1')

function Invoke-ProbeCommand([string]$Executable, [string[]]$Arguments, [string]$Log) {
    & $Executable @Arguments > $Log 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Probe command failed ($LASTEXITCODE): $Executable; log: $Log" }
}

function Find-ScriptNode {
    $candidates = @($env:HE_SCRIPT_NODE,
        (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe'),
        (Get-Command node.exe -ErrorAction SilentlyContinue).Source)
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            & $candidate -e 'process.exit(parseInt(process.versions.node) >= 18 ? 0 : 1)'
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
    throw 'Set HE_SCRIPT_NODE to Node.js 18 or later.'
}

if ($Action -eq 'serve-web') {
    & (Find-ScriptNode) (Join-Path $PSScriptRoot 'serve-web.cjs') (Join-Path $work 'game-web')
    return
}
if ($Action -eq 'build-wechat') {
    & (Join-Path $PSScriptRoot 'BuildWebGame.ps1') -Platform WeChat
    Invoke-ProbeCommand (Find-ScriptNode) @((Join-Path $PSScriptRoot 'export-wechat.cjs')) (Join-Path $work 'wechat-game-export.json')
    Write-Host 'build-only: hectl scripting build-wechat PASS; open out/agent/scripting/game-wechat in DevTools.'
    return
}
Push-Location $probe
try {
    $dotnet = (Get-Command dotnet.exe -ErrorAction Stop).Source
    $version = & $dotnet --version
    if ($LASTEXITCODE -ne 0 -or $version -ne '10.0.302') { throw 'The scripting probe requires .NET SDK 10.0.302.' }
    if ($Action -eq 'setup') {
        Invoke-ProbeCommand $dotnet @('workload','install','wasm-tools','--skip-manifest-update') (Join-Path $work 'workload-install.log')
        $node = Find-ScriptNode
        $npmRoot = Split-Path (Get-Command npm.cmd -ErrorAction Stop).Source
        $npmCli = Join-Path $npmRoot 'node_modules/npm/bin/npm-cli.js'
        Push-Location $PSScriptRoot
        try {
            Invoke-ProbeCommand $node @($npmCli,'ci','--ignore-scripts','--include=optional','--no-audit','--no-fund',
                '--registry=https://registry.npmjs.org','--fetch-timeout=60000','--fetch-retries=1') (Join-Path $work 'npm-install.log')
        } finally { Pop-Location }
    }
    elseif ($Action -eq 'probe-pc') {
        Invoke-ProbeCommand $dotnet @('build','Probe.Pc.csproj','-c','Release','-o',(Join-Path $work 'probe-pc-managed')) (Join-Path $work 'pc-managed-build.log')
        $toolchain = Get-HeVisualStudio
        Import-HeToolchain -Toolchain $toolchain
        $cmake = (Get-Command cmake.exe).Source
        $native = Join-Path $work 'probe-pc-native'
        Invoke-ProbeCommand $cmake @('-S',$probe,'-B',$native,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release') (Join-Path $work 'pc-native-configure.log')
        Invoke-ProbeCommand $cmake @('--build',$native) (Join-Path $work 'pc-native-build.log')
        Invoke-ProbeCommand (Join-Path $native 'HEngineScriptCoreContracts.exe') @() (Join-Path $work 'core-isolated-runtime.log')
        Write-Host 'unit-test isolated ScriptCore: PASS'
        $hostFxr = Join-Path (Split-Path $dotnet) 'host/fxr/10.0.10/hostfxr.dll'
        Invoke-ProbeCommand (Join-Path $native 'HEngineManagedProbe.exe') @($hostFxr,
            (Join-Path $work 'probe-pc-managed/HEngine.ManagedProbe.runtimeconfig.json'),
            (Join-Path $work 'probe-pc-managed/HEngine.ManagedProbe.dll')) (Join-Path $work 'pc-host-runtime.log')
        Write-Host 'runtime-manual probe-pc: PASS (native CoreCLR calls; not HGame integration)'
    }
    else {
        Invoke-ProbeCommand $dotnet @('publish','Probe.csproj','-c','Release','-o',(Join-Path $work 'probe-web')) (Join-Path $work 'probe-build.log')
        if ($Action -eq 'probe-wechat') {
            $node = Find-ScriptNode
            $runtimePack = Join-Path (Split-Path $dotnet) 'packs/Microsoft.NETCore.App.Runtime.Mono.browser-wasm/10.0.10'
            $previousPack = $env:HE_SCRIPT_RUNTIME_PACK
            try {
                $env:HE_SCRIPT_RUNTIME_PACK = $runtimePack
                Invoke-ProbeCommand $node @((Join-Path $PSScriptRoot 'export-probe.cjs')) (Join-Path $work 'probe-export.log')
            } finally { $env:HE_SCRIPT_RUNTIME_PACK = $previousPack }
        }
        Write-Host "build-only $Action`: PASS; manual browser/DevTools verification still required"
    }
} finally { Pop-Location }
