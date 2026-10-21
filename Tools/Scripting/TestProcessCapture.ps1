param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$work = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo ('out/agent/scripting/process-capture-' + (Get-Date -Format yyyyMMdd-HHmmss-fff)) }
New-Item -ItemType Directory -Force $work | Out-Null
. "$repo/Tools/Agent/Storage.ps1"
$record = if (!$OutputDirectory) { Start-HEStorageRun -Project "$repo/Project" -Workflow 'process-capture-contract' -Paths @($work) -Outer } else { $null }
$state = 'failed'
. "$repo/Tools/Agent/Toolchain.ps1"
Import-HeToolchain -Toolchain (Get-HeVisualStudio)
Push-Location $work
try {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX "/I$repo/Engine/Source" `
        "$repo/Tools/Scripting/ProcessCaptureTests.cpp" "$repo/Engine/Source/Runtime/Platform/Windows/WindowsPlatformProcess.cpp" `
        /Fe:ProcessCaptureTests.exe *> build.log
    if ($LASTEXITCODE -ne 0) { throw "Process capture test build failed: $work/build.log" }
    & ./ProcessCaptureTests.exe *> result.log
    if ($LASTEXITCODE -ne 0) { throw "Process capture contract failed: $work/result.log" }
    Get-Content result.log
    $state = 'success'
} finally { Pop-Location; if ($record) { Complete-HEStorageRun -Run $record -State $state } }
