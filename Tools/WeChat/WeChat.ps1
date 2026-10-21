param(
    [ValidateSet('doctor','setup','validate','shaders','build','export','check')]
    [string]$Action = 'doctor',
    [string]$ProjectRoot,
    [string]$Config = '',
    [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$work = Join-Path $repository 'out/agent/wechat'
$deps = Join-Path $work 'deps'
$evidence = Join-Path $work 'evidence'
$lock = Get-Content (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $deps,$evidence | Out-Null
if (-not $Config) { $Config = Join-Path $ProjectRoot 'Config/WeChat.json' }
if (-not $Output) { $Output = Join-Path $repository 'out/agent/wechat-export' }
if ((Resolve-Path $ProjectRoot).Path -ne (Join-Path $repository 'Project')) {
    throw 'The initial C++ sample profile supports this repository Project only.'
}

function Find-Python {
    $candidates = @($env:HE_WECHAT_PYTHON)
    if (Get-Command py -ErrorAction SilentlyContinue) { $candidates += @(& py -3 -c 'import sys; print(sys.executable)' 2>$null) }
    $candidates += @((Get-Command python -ErrorAction SilentlyContinue).Source)
    $candidates += Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            & $candidate -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>$null
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
    throw 'Set HE_WECHAT_PYTHON to a Python 3.10+ host interpreter; Python is never linked into Wasm.'
}
function Find-Node {
    $candidates = @($env:HE_WECHAT_NODE, (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe'), (Get-Command node -ErrorAction SilentlyContinue).Source)
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            & $candidate -e 'process.exit(parseInt(process.versions.node)>=20?0:1)'
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
    throw 'Set HE_WECHAT_NODE to a Node.js 20+ executable.'
}
function Get-DependencyHash([string]$FilePath) {
    $stream = [System.IO.File]::OpenRead($FilePath)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
function Verify-Hash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path) -or (Get-DependencyHash $Path) -ne $Expected) {
        throw "Pinned dependency hash mismatch or missing file: $Path"
    }
}
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$Log) {
    $null = Get-Command $Exe -ErrorAction Stop
    # Windows PowerShell otherwise turns expected native stderr diagnostics
    # into terminating errors even when a contract test exits successfully.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Exe @Arguments > $Log 2>&1
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) { throw "Command failed (exit $nativeExit): $Exe; full log: $Log" }
}
$python = Find-Python
$node = Find-Node
$sdk = Join-Path $deps 'emsdk/upstream/install'
$emcc = Join-Path $sdk 'emscripten/emcc.bat'
$cross = if ($env:HE_WECHAT_SPIRV_CROSS) { $env:HE_WECHAT_SPIRV_CROSS } else { (Get-Command spirv-cross -ErrorAction SilentlyContinue).Source }
$dxc = Join-Path $repository 'Engine/Source/Programs/HShaderCompiler/ThirdParty/dxc/binary/x64/dxc.exe'

if ($Action -eq 'doctor') {
    [ordered]@{python=$python;node=$node;emscripten=$emcc;emscriptenInstalled=(Test-Path $emcc);dxc=$dxc;spirvCross=$cross;config=$Config;output=$Output;wechatValidation='Requires logged-in DevTools and a configured AppID; never uploads'} | ConvertTo-Json
    exit 0
}
if ($Action -eq 'setup') {
    $archive = Join-Path $deps 'emsdk/downloads/wasm-binaries.zip'
    New-Item -ItemType Directory -Force (Split-Path $archive) | Out-Null
    if (-not (Test-Path $archive) -or (Get-DependencyHash $archive) -ne $lock.emscripten.sha256) {
        & curl.exe -L --fail --retry 3 --continue-at - --max-time 1800 $lock.emscripten.url -o $archive
        if ($LASTEXITCODE -ne 0) { throw 'Emscripten download interrupted; rerun hectl wechat setup to resume.' }
    }
    Verify-Hash $archive $lock.emscripten.sha256
    if (-not (Test-Path $emcc)) {
        & $python -m zipfile -e $archive (Join-Path $deps 'emsdk/upstream')
        if ($LASTEXITCODE -ne 0) { throw 'Emscripten extraction failed' }
    }
    foreach ($file in @('sokol_gfx.h','LICENSE')) {
        $target = Join-Path $deps $(if ($file -eq 'LICENSE') { 'sokol-LICENSE' } else { $file })
        & curl.exe -L --fail --retry 3 --max-time 90 "https://raw.githubusercontent.com/floooh/sokol/$($lock.sokol.commit)/$file" -o $target
        if ($LASTEXITCODE -ne 0) { throw 'Sokol download failed' }
    }
    Verify-Hash (Join-Path $deps 'sokol_gfx.h') $lock.sokol.sha256
    Write-Output 'PASS pinned Emscripten and Sokol dependency setup'
    exit 0
}
if (-not (Test-Path $Config)) { throw "Missing WeChat config: $Config" }
$exporter = Join-Path $PSScriptRoot 'export_wechat.py'
$exportArgs = @('--root',$repository,'--project',$ProjectRoot,'--config',$Config,'--output',$Output)
Invoke-Logged $python (@($exporter,'validate') + $exportArgs) (Join-Path $evidence 'export-validation.log')
if ($Action -eq 'validate') { Write-Output 'PASS strict WeChat scene/resource capability validation'; exit 0 }
if (-not $cross) { throw 'Set HE_WECHAT_SPIRV_CROSS to the pinned SPIRV-Cross executable; see dependencies.lock.json.' }
Verify-Hash $dxc $lock.dxc.exeSha256
Verify-Hash (Join-Path (Split-Path $dxc) 'dxcompiler.dll') $lock.dxc.dllSha256
Verify-Hash $cross $lock.spirvCross.exeSha256
$shaderRoot = Join-Path $work 'shaders'
Invoke-Logged $python @((Join-Path $repository 'Engine/Source/Programs/HShaderCompiler/compile_webgl.py'),'--root',$repository,'--output',$shaderRoot,'--dxc',$dxc,'--cross',$cross) (Join-Path $evidence 'shader-build.log')
if ($Action -eq 'shaders') { Write-Output "PASS offline WebGL Shader pipeline: $shaderRoot"; exit 0 }
if (-not (Test-Path $emcc)) { throw 'Run hectl wechat setup first.' }
Verify-Hash (Join-Path $deps 'sokol_gfx.h') $lock.sokol.sha256
if ((Get-Content (Join-Path $sdk 'emscripten/emscripten-version.txt') -Raw).Trim() -ne $lock.emscripten.version) { throw 'Emscripten version mismatch' }
$env:EMSDK_PYTHON = $python
$env:EM_CONFIG = Join-Path $deps 'emscripten.config.py'
$sdkSlash = $sdk.Replace('\','/'); $nodeSlash = $node.Replace('\','/')
@("LLVM_ROOT = '$sdkSlash/bin'", "BINARYEN_ROOT = '$sdkSlash'", "NODE_JS = '$nodeSlash'", "CACHE = '$sdkSlash/emscripten/cache'") | Set-Content $env:EM_CONFIG
. (Join-Path $repository 'Tools/Agent/Toolchain.ps1')
Import-HeToolchain
Invoke-Logged (Join-Path $repository 'hectl.bat') @('codegen','run','-CodeGenHost','DevGame','-Configuration','RelWithDebInfo','--json') (Join-Path $evidence 'codegen.json')
$generated = Join-Path $repository 'out/agent/game-relwithdebinfo/GeneratedSource'
$build = Join-Path $repository 'out/agent/wechat-wasm-release'
$common = @('-S',$repository,'-G','Ninja','-DHE_BUILD_WECHAT=ON','-DHE_BUILD_WEB=OFF','-DHE_SCRIPT_CSHARP_WASM=OFF','-DCMAKE_BUILD_TYPE=Release',"-DHE_WECHAT_CODEGEN_ROOT=$generated","-DHE_WECHAT_SHADER_ROOT=$shaderRoot","-DHE_WECHAT_DEPENDENCIES=$deps")
Invoke-Logged 'cmake' (@($common) + @('-B',$build,"-DCMAKE_TOOLCHAIN_FILE=$sdk/emscripten/cmake/Modules/Platform/Emscripten.cmake")) (Join-Path $evidence 'wasm-configure.log')
Invoke-Logged 'cmake' @('--build',$build,'-j','4') (Join-Path $evidence 'wasm-build.log')
Invoke-Logged (Join-Path $sdk 'bin/wasm-opt.exe') @((Join-Path $build 'hegame.wasm'),'--mvp-features','-o',(Join-Path $evidence 'mvp-validation.wasm')) (Join-Path $evidence 'wasm-features.log')
Invoke-Logged $node @((Join-Path $PSScriptRoot 'test_wasm.cjs'),$build) (Join-Path $evidence 'wasm-no-dom.log')
Invoke-Logged $python @((Join-Path $PSScriptRoot 'build_receipt.py'),$build,$shaderRoot) (Join-Path $evidence 'dependency-audit.log')
Write-Output "PASS Wasm build: $build"
if ($Action -in @('export','check')) {
    $nativeBuild = Join-Path $repository 'out/agent/wechat-native-release'
    Invoke-Logged 'cmake' (@($common) + @('-B',$nativeBuild)) (Join-Path $evidence 'native-configure.log')
    Invoke-Logged 'cmake' @('--build',$nativeBuild,'-j','4') (Join-Path $evidence 'native-build.log')
    $profile = Get-Content $Config -Raw | ConvertFrom-Json
    Invoke-Logged (Join-Path $nativeBuild 'HEngineWeChat.exe') @('--validate-scene',$ProjectRoot,$profile.scene,$profile.texture) (Join-Path $evidence 'native-scene-validation.log')
    Invoke-Logged $python (@($exporter,'export') + $exportArgs) (Join-Path $evidence 'export.json')
    Write-Output "PASS local WeChat export: $Output"
}
if ($Action -eq 'check') {
    Invoke-Logged (Join-Path $nativeBuild 'HEngineWeChat.exe') @($ProjectRoot,'Assets/WeChatSample/Touch.he','Assets/WeChatSample/checker.png') (Join-Path $evidence 'native-contracts.log')
    Invoke-Logged $python @((Join-Path $PSScriptRoot 'test_export.py')) (Join-Path $evidence 'export-tests.log')
    Invoke-Logged $node @((Join-Path $PSScriptRoot 'test_host.cjs')) (Join-Path $evidence 'host-tests.log')
    Invoke-Logged $node @((Join-Path $PSScriptRoot 'test_boot.cjs')) (Join-Path $evidence 'boot-tests.log')
    Write-Output 'PASS native optimized contracts, strict export failures and JavaScript host tests. This is not WeChat device evidence.'
}
