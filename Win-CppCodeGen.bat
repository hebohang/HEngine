@echo off
setlocal EnableExtensions

for %%I in ("%~dp0.") do set "REPO_ROOT=%%~fI"
set "CODEGEN_PROJECT=%HE_PROJECT_DIR%"
set "GENERATED_SOURCE_DIR="
set "CODEGEN_ACTION=Run"
set "CODEGEN_HOST=Editor"
set "CODEGEN_CONFIGURATION=Debug"
set "NO_PAUSE="
set "JSON_ARG="
set "QUIET_ARG="

:parse_args
if "%~1"=="" goto :args_parsed
if /I "%~1"=="status" (
  set "CODEGEN_ACTION=Status"
  shift
  goto :parse_args
)
if /I "%~1"=="check" (
  set "CODEGEN_ACTION=Check"
  shift
  goto :parse_args
)
if /I "%~1"=="run" (
  set "CODEGEN_ACTION=Run"
  shift
  goto :parse_args
)
if /I "%~1"=="--no-pause" (
  set "NO_PAUSE=1"
  shift
  goto :parse_args
)
if /I "%~1"=="--json" (
  set "JSON_ARG=-Json"
  shift
  goto :parse_args
)
if /I "%~1"=="--quiet" (
  set "QUIET_ARG=-Quiet"
  shift
  goto :parse_args
)
if /I "%~1"=="-Project" (
  if "%~2"=="" goto :missing_project
  set "CODEGEN_PROJECT=%~2"
  shift
  shift
  goto :parse_args
)
if /I "%~1"=="-GeneratedSourceDir" (
  if "%~2"=="" goto :missing_output
  set "GENERATED_SOURCE_DIR=%~2"
  shift
  shift
  goto :parse_args
)
if /I "%~1"=="-Host" (
  if "%~2"=="" goto :missing_host
  set "CODEGEN_HOST=%~2"
  shift
  shift
  goto :parse_args
)
if /I "%~1"=="-Configuration" (
  if "%~2"=="" goto :missing_configuration
  set "CODEGEN_CONFIGURATION=%~2"
  shift
  shift
  goto :parse_args
)
echo [Win-CppCodeGen] Unknown argument: %~1 1>&2
goto :error

:args_parsed
if not defined CODEGEN_PROJECT set "CODEGEN_PROJECT=%REPO_ROOT%\Project"
if not defined GENERATED_SOURCE_DIR set "GENERATED_SOURCE_DIR=%REPO_ROOT%\build\GeneratedSource"
set "CODEGEN_PROJECT_ROOT="
for /f "usebackq delims=" %%A in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $item = Get-Item -LiteralPath $env:CODEGEN_PROJECT -Force; if ($item.PSIsContainer) { $root = $item.FullName } else { if ($item.Name -ine 'Project.heproject') { throw 'Project path must name a directory or Project.heproject.' }; $root = $item.Directory.FullName }; $descriptor = Join-Path $root 'Project.heproject'; if (-not (Test-Path -LiteralPath $descriptor -PathType Leaf)) { throw ('Project descriptor was not found: ' + $descriptor) }; [IO.Path]::GetFullPath($root).TrimEnd('\', '/')"`) do set "CODEGEN_PROJECT_ROOT=%%A"
if not defined CODEGEN_PROJECT_ROOT (
  echo [Win-CppCodeGen] Failed to resolve project: %CODEGEN_PROJECT% 1>&2
  goto :error
)
set "CODEGEN_PROJECT=%CODEGEN_PROJECT_ROOT%"
for /f "usebackq delims=" %%A in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "[IO.Path]::GetFullPath($env:GENERATED_SOURCE_DIR)"`) do set "GENERATED_SOURCE_DIR=%%A"
if not defined GENERATED_SOURCE_DIR (
  echo [Win-CppCodeGen] Failed to resolve generated source directory. 1>&2
  goto :error
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\Tools\Agent\CodeGen.ps1" ^
  -RepositoryRoot "%REPO_ROOT%" ^
  -ProjectRoot "%CODEGEN_PROJECT%" ^
  -GeneratedSourceDir "%GENERATED_SOURCE_DIR%" ^
  -Action "%CODEGEN_ACTION%" ^
  -CodeGenHost "%CODEGEN_HOST%" ^
  -Configuration "%CODEGEN_CONFIGURATION%" ^
  %JSON_ARG% %QUIET_ARG%
if errorlevel 1 goto :error

if not defined NO_PAUSE pause
exit /b 0

:missing_project
echo [Win-CppCodeGen] -Project requires a project directory or Project.heproject. 1>&2
goto :error

:missing_output
echo [Win-CppCodeGen] -GeneratedSourceDir requires a build-local directory. 1>&2
goto :error

:missing_host
echo [Win-CppCodeGen] -Host requires Editor, DevGame, Tests, or Package. 1>&2
goto :error

:missing_configuration
echo [Win-CppCodeGen] -Configuration requires Debug, RelWithDebInfo, or Release. 1>&2
goto :error

:error
if not defined NO_PAUSE pause
exit /b 1
