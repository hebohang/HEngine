param(
    [ValidateSet('Debug','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateSet('DX12','Vulkan')][string]$GraphicsApi = 'DX12',
    [string]$Python311 = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $Python311) {
    $Python311 = (& py -3.11 -c 'import sys; print(sys.executable)').Trim()
    if ($LASTEXITCODE -ne 0) { throw 'A complete developer CPython 3.11 installation is required.' }
}
$details = & $Python311 -c 'import json,sys,sysconfig,os; print(json.dumps({"version":"%d.%d"%sys.version_info[:2],"stdlib":sysconfig.get_path("stdlib"),"dlls":os.path.join(sys.base_prefix,"DLLs")}))'
if ($LASTEXITCODE -ne 0) { throw 'Failed to inspect developer Python.' }
$pythonInfo = $details | ConvertFrom-Json
if ($pythonInfo.version -ne '3.11') { throw 'Expected CPython 3.11 to match the embedded ABI.' }
$names = @('HE_DEV_PYTHON_STDLIB','HE_DEV_PYTHON_DLLS','HE_DEV_PYTHON_VERSION')
$old = @{}
foreach ($name in $names) { $old[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$resultPath = Join-Path $root 'out/agent/python-stubs/runtime-result.json'
$evidence = Join-Path $root ('out/agent/logs/python-stubs/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $evidence | Out-Null
. (Join-Path $root 'Tools/Agent/Storage.ps1')
$storage = Start-HEStorageRun -Project (Join-Path $root 'Project') -Workflow 'python-stub-acceptance' -Paths @($evidence) -Outer
$state = 'failed'
$started = [DateTime]::UtcNow
try {
    $env:HE_DEV_PYTHON_STDLIB = $pythonInfo.stdlib
    $env:HE_DEV_PYTHON_DLLS = $pythonInfo.dlls
    $env:HE_DEV_PYTHON_VERSION = $pythonInfo.version
    & (Join-Path $root 'hectl.bat') game check -Configuration $Configuration -GraphicsApi $GraphicsApi -StrictGraphicsApi -StartupScene Assets/Scenes/PythonStubGeneration.he -Frames 15
    if ($LASTEXITCODE -ne 0) { throw 'HEngine process check failed.' }
    if (-not (Test-Path -LiteralPath $resultPath) -or (Get-Item -LiteralPath $resultPath).LastWriteTimeUtc -lt $started) {
        throw 'Current embedded Python acceptance did not produce a result.'
    }
    $result = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
    if ($result.status -ne 'PASS') { throw ('Embedded Python acceptance failed: ' + ($result | ConvertTo-Json -Depth 8 -Compress)) }
    & $Python311 (Join-Path $PSScriptRoot 'CheckPythonStubs.py')
    if ($LASTEXITCODE -ne 0) { throw 'Generated stubs did not pass freshness/coverage checks.' }
    $state = 'success'
    Write-Output 'PASS: current embedded API execution and generated stubs.'
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$old[$name],'Process') }
    if ((Test-Path -LiteralPath $resultPath) -and (Get-Item -LiteralPath $resultPath).LastWriteTimeUtc -ge $started) {
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $evidence 'runtime-result.json')
    }
    Complete-HEStorageRun -Run $storage -State $state
}
