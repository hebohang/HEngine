param([switch]$CheckOnly, [switch]$WhatIf, [switch]$Offline, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$installExitCode = 1
try {
    if (!(Test-Path -LiteralPath "$repo/CMakeLists.txt") -or (Test-Path -LiteralPath "$repo/HEngine.distribution.json")) {
        throw 'Run this launcher from an HEngine source checkout.'
    }
    Write-Host 'HEngine Windows x64 source build dependencies'
    Write-Host 'Manual prerequisite: Visual Studio 2026 / v145 (or VS2022 / v143).'
    Write-Host 'Select Desktop development with C++, x64/x86 MSVC, C++ CMake tools (Ninja),'
    Write-Host 'and Windows SDK 10.0.26100 in Visual Studio Installer.'
    Write-Host 'Visual Studio is not installed or modified by this launcher.'
    Write-Host 'Other missing dependencies use the repository version locks and verified downloads.'
    Write-Host ''
    $action = if ($CheckOnly) { 'check' } else { 'install' }
    # Use a child process so the dependency entry point's exit code cannot skip
    # the launcher's interactive failure summary and pause.
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/Dependencies.ps1",
        '-Action',$action,'-Profile','Source','-Project',"$repo/Engine/Templates/Blank")
    if ($Offline) { $arguments += '-Offline' }
    if ($WhatIf) { $arguments += '-WhatIf' }
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe @arguments
        $installExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($installExitCode -eq 0) {
        if ($WhatIf) { Write-Host 'Preview complete; no dependencies were installed. Failed checks above identify missing prerequisites.' }
        else { Write-Host 'Source build dependency check PASS. Run Win-GenProjects.bat to configure and build.' }
    } else {
        Write-Host 'Source build dependencies are incomplete. Resolve failed checks above and rerun this BAT.' -ForegroundColor Red
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
} finally {
    if (!$NoPause) { $null = Read-Host 'Press Enter to close' }
}
exit $installExitCode
