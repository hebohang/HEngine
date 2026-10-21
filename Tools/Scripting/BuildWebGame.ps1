param([ValidateSet('Web','WeChat')][string]$Platform = 'Web')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$work = Join-Path $repo 'out/agent/scripting'
$stage = Join-Path $work ($Platform.ToLowerInvariant() + '-managed-stage')
$native = Join-Path $repo 'out/agent/web-script-release'
$deps = Join-Path $repo 'out/agent/wechat/deps'
$sdk = Join-Path $deps 'emsdk/upstream/install'
. (Join-Path $repo 'Tools/Agent/Toolchain.ps1')
Import-HeToolchain -Toolchain (Get-HeVisualStudio)
function Invoke-WebBuild([string]$Exe, [string[]]$Arguments, [string]$Log) {
    $previous = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $Exe @Arguments *> (Join-Path $work $Log); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    if ($code -ne 0) { throw "Web build failed ($code); see out/agent/scripting/$Log" }
}
function Find-WebTool([string[]]$Candidates, [string]$Label) {
    foreach ($candidate in $Candidates) { if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate } }
    throw "Missing $Label; configure the HE_SCRIPT_NODE / HE_WECHAT_PYTHON host tool paths."
}
$node = Find-WebTool @($env:HE_SCRIPT_NODE, (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe'), (Get-Command node.exe -ErrorAction SilentlyContinue).Source) 'Node.js'
$python = Find-WebTool @($env:HE_WECHAT_PYTHON, (Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'), (Get-Command python.exe -ErrorAction SilentlyContinue).Source) 'Python build host'
New-Item -ItemType Directory -Force $work | Out-Null
Push-Location $repo
$oldPython = $env:EMSDK_PYTHON; $oldConfig = $env:EM_CONFIG; $oldPack = $env:HE_SCRIPT_RUNTIME_PACK
try {
    $lock = Get-Content (Join-Path $repo 'Tools/WeChat/dependencies.lock.json') -Raw | ConvertFrom-Json
    if (!(Test-Path (Join-Path $sdk 'emscripten/emcc.py'))) { throw 'Run hectl wechat setup and hectl scripting setup first.' }
    if ((Get-Content (Join-Path $sdk 'emscripten/emscripten-version.txt') -Raw).Trim() -ne $lock.emscripten.version) { throw 'Pinned Emscripten version mismatch.' }
    $bytes = [IO.File]::ReadAllBytes((Join-Path $deps 'sokol_gfx.h'))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-','').ToLowerInvariant() } finally { $hasher.Dispose() }
    if ($hash -ne $lock.sokol.sha256) { throw 'Pinned Sokol integrity mismatch.' }
    Invoke-WebBuild (Join-Path $repo 'hectl.bat') @('wechat','shaders') 'web-shaders.log'
    Invoke-WebBuild (Join-Path $repo 'hectl.bat') @('codegen','run','-CodeGenHost','DevGame','-Configuration','RelWithDebInfo','--json') 'web-codegen.json'
    $shell = (Get-Process -Id $PID).Path
    Invoke-WebBuild $shell @('-NoProfile','-File',(Join-Path $PSScriptRoot 'BuildManaged.ps1'),'-Project',(Join-Path $repo 'Project'),'-WorkDirectory',$stage,'-OutputDirectory',(Join-Path $stage 'output'),'-Configuration','Release','-Platform',$Platform,'-Quality','Portable','-CMake',(Get-Command cmake.exe).Source) 'web-managed-stage.log'
    $moduleArguments = @()
    if (@(Get-ChildItem -LiteralPath "$repo/Project/Scripts" -Filter '*.hemodule' -Recurse -File).Count -gt 0) {
        $moduleArguments = @("-p:ModuleReferencesFile=$stage/modules/ModuleReferences.props", "-p:GameScriptsProject=$stage/modules/GameScripts.csproj")
    }
    Push-Location (Join-Path $repo 'Engine/Managed')
    try {
        Invoke-WebBuild 'dotnet' (@('publish','HEngine.ScriptWasm/HEngine.ScriptWasm.csproj','-c','Release',"-p:HEPlatform=$Platform",'-p:HEQuality=Portable','--artifacts-path',(Join-Path $stage 'wasm-artifacts'),"-p:ScriptRegistryDir=$stage/registry","-p:GeneratedApiDir=$stage/api",'-o',(Join-Path $work 'game-web-managed')) + $moduleArguments) 'wasm-managed-publish.log'
    } finally { Pop-Location }
    $env:EMSDK_PYTHON = $python
    $env:EM_CONFIG = Join-Path $deps 'emscripten.config.py'
    $sdkSlash = $sdk.Replace('\','/'); $nodeSlash = $node.Replace('\','/')
    @("LLVM_ROOT = '$sdkSlash/bin'", "BINARYEN_ROOT = '$sdkSlash'", "NODE_JS = '$nodeSlash'", "CACHE = '$sdkSlash/emscripten/cache'") | Set-Content -LiteralPath $env:EM_CONFIG
    Invoke-WebBuild 'cmake' @('-S',$repo,'-B',$native,'-G','Ninja','-DHE_BUILD_WEB=ON','-DHE_SCRIPT_CSHARP_WASM=ON','-DCMAKE_BUILD_TYPE=Release',"-DHE_WECHAT_CODEGEN_ROOT=$repo/out/agent/game-relwithdebinfo/GeneratedSource","-DHE_WECHAT_SHADER_ROOT=$repo/out/agent/wechat/shaders","-DHE_WECHAT_DEPENDENCIES=$deps","-DCMAKE_TOOLCHAIN_FILE=$sdk/emscripten/cmake/Modules/Platform/Emscripten.cmake") 'web-engine-configure.log'
    Invoke-WebBuild 'cmake' @('--build',$native,'--target','HEngineWeb','--parallel','4') 'web-engine-build.log'
    $env:HE_SCRIPT_RUNTIME_PACK = Join-Path (Split-Path (Get-Command dotnet.exe).Source) 'packs/Microsoft.NETCore.App.Runtime.Mono.browser-wasm/10.0.10'
    Invoke-WebBuild $node @((Join-Path $PSScriptRoot 'export-web.cjs'),(Join-Path $work 'game-web-managed'),$native,(Join-Path $stage 'registry'),(Join-Path $work 'game-web')) 'web-export.json'
    Write-Host 'build-only: hectl scripting build-web PASS'
    Write-Host 'Serve: hectl scripting serve-web (http://127.0.0.1:8768/)'
} finally {
    $env:EMSDK_PYTHON = $oldPython; $env:EM_CONFIG = $oldConfig; $env:HE_SCRIPT_RUNTIME_PACK = $oldPack
    Pop-Location
}
