param([string]$Root='', [switch]$DownloadOnly)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. "$PSScriptRoot/Environment.ps1"
Import-HEDependencyEnvironment
if (!$Root) { $Root=Join-Path $repo 'out/agent/dependencies/windows-arm64' }
$Root=[IO.Path]::GetFullPath($Root)
$python=if($env:HE_BUILD_PYTHON){$env:HE_BUILD_PYTHON}else{(Get-Command python.exe -ErrorAction Stop).Source}
& $python "$PSScriptRoot/prepare_arm64.py" --root $Root
if ($LASTEXITCODE -ne 0) { throw 'Pinned ARM64 dependency acquisition failed.' }
$paths=Get-Content "$Root/prepared.json" -Raw | ConvertFrom-Json
[IO.File]::WriteAllText("$Root/.he-keep", 'Active ARM64 dependency SDK; preserve while builds consume it.')
. "$repo/Tools/Packaging/PeImage.ps1"
foreach ($file in @("$($paths.agility)/build/native/bin/arm64/D3D12Core.dll",
                    "$($paths.dxc)/build/native/bin/arm64/dxc.exe", "$($paths.python)/tools/python311.dll")) {
    if ((Get-PeImage $file).Machine -ne 0xaa64) { throw "Dependency is not native ARM64: $file" }
}
$runtime=& "$repo/Tools/Scripting/GetPinnedRuntime.ps1" -Architecture arm64 -CacheDirectory "$Root/dotnet"
$paths | Add-Member -NotePropertyName runtimeRoot -NotePropertyValue $runtime.Root -Force
$paths | ConvertTo-Json | Set-Content "$Root/prepared.json" -Encoding UTF8
$env:HE_WINDOWS_ARM64_ROOT=$Root
$env:HE_D3D12_AGILITY_SDK_SOURCE_DIR=$paths.agility
$env:HE_ARM64_DXC_ROOT="$($paths.dxc)/build/native/bin/arm64"
$env:HE_ARM64_PYTHON_ROOT="$($paths.python)/tools"
$env:HE_ARM64_RUNTIME_ROOT=$runtime.Root
if ($DownloadOnly) { Write-Output 'PASS: ARM64 input hashes and PE architecture; native compilation was not requested.'; return }
. "$repo/Tools/Agent/Toolchain.ps1"
$armToolchain=Get-HeVisualStudio -Architecture arm64
Import-HeToolchain -Toolchain $armToolchain
function Run([string]$Program, [string[]]$Arguments) {
    $previousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $Program @Arguments
        $code=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previousPreference }
    if ($code -ne 0) { throw "ARM64 preparation failed: $Program (exit $code)" }
}
# SDK 1.1.4 has an official DX12_ARM64 backend. Build its API DLL from the
# hash-pinned archive; never relabel the signed x64 prebuilt DLL as ARM64.
Run 'cmake' @('-S',"$($paths.fsr)/ffx-api",'-B',"$Root/fsr-build",'-G',$armToolchain.Generator,
    '-A','arm64','-T',"$($armToolchain.Toolset),host=$($armToolchain.HostArchitecture)",
    '-DFFX_API_BACKEND=DX12_ARM64','-DFFX_API_DX12=ON','-DFFX_API_VK=OFF',
    "-DCMAKE_GENERATOR_INSTANCE=$($armToolchain.Path)")
Run 'cmake' @('--build',"$Root/fsr-build",'--config','Release','--target','amd_fidelityfx_dx12','--parallel','2')
$fsrDll="$($paths.fsr)/ffx-api/bin/amd_fidelityfx_dx12.dll"
if ((Get-PeImage $fsrDll).Machine -ne 0xaa64) { throw 'FSR source build did not produce native ARM64.' }
$fsrLock=Get-Content "$PSScriptRoot/native-runtime.arm64.lock.json" -Raw | ConvertFrom-Json
$fsrReceipt=[ordered]@{schema=1;architecture='arm64';backend='dx12';sdkVersion='1.1.4';
    sourceArchiveSha256=$fsrLock.fsrSourceArchiveSha256;
    runtimeSha256=(Get-FileHash -LiteralPath $fsrDll -Algorithm SHA256).Hash.ToLowerInvariant();
    compilerVersion=$armToolchain.ToolsVersion}
$fsrReceipt | ConvertTo-Json | Set-Content "$Root/fsr-build/HEngine.FsrBuild.json" -Encoding UTF8
$null=New-Item -ItemType Directory -Force "$Root/vulkan/lib"
Run 'lib.exe' @('/nologo','/machine:ARM64',"/def:$($paths.'vulkan-def')","/out:$Root/vulkan/lib/vulkan-1.lib")
Run 'cmake' @('-S',$paths.embree,'-B',"$Root/embree-build",'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_SYSTEM_NAME=Windows','-DCMAKE_SYSTEM_PROCESSOR=ARM64',"-DCMAKE_INSTALL_PREFIX=$Root/embree",'-DEMBREE_STATIC_LIB=ON',
    '-DEMBREE_STATIC_RUNTIME=ON','-DEMBREE_TASKING_SYSTEM=INTERNAL','-DEMBREE_ISPC_SUPPORT=OFF',
    '-DEMBREE_TUTORIALS=OFF','-DEMBREE_SYCL_SUPPORT=OFF','-DEMBREE_TESTING_INTENSITY=0')
Run 'cmake' @('--build',"$Root/embree-build",'--target','install','--parallel','2')
# Reflection parsing is a build-host process, not a target runtime dependency.
# The existing pinned x64 libclang can run under Windows on Arm emulation.
$savedTarget=$env:HE_TARGET_ARCHITECTURE
try {
    $env:HE_TARGET_ARCHITECTURE='x64'
    $hostToolchain=Get-HeVisualStudio -Architecture x64
    Import-HeToolchain -Toolchain $hostToolchain
    Run 'cmake' @('-S',$repo,'-B',"$Root/host-parser-build",'-G','Ninja',
        '-DHE_CODEGEN_TOOL_ONLY=ON','-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$($hostToolchain.Compiler)","-DCMAKE_CXX_COMPILER=$($hostToolchain.Compiler)")
    Run 'cmake' @('--build',"$Root/host-parser-build",'--target','HEngineParser','--parallel','2')
} finally { $env:HE_TARGET_ARCHITECTURE=$savedTarget }
$hostParser="$Root/host-parser-build/bin/Programs/HEngineParser"
& robocopy.exe $hostParser "$Root/host-parser" /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XF *.pdb *.ilk *.lib *.exp | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Could not preserve the host parser payload.' }
$env:HE_CODEGEN_PARSER_EXECUTABLE="$Root/host-parser/HEngineParser.exe"
if (!(Test-Path -LiteralPath $env:HE_CODEGEN_PARSER_EXECUTABLE)) { throw 'Host parser output was not found.' }
Import-HeToolchain -Toolchain (Get-HeVisualStudio -Architecture arm64)
Write-Output 'PASS: pinned ARM64 dependencies and host code generator prepared.'
