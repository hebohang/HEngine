@echo off
setlocal EnableExtensions

for %%I in ("%~dp0.") do set "REPO_ROOT=%%~fI"
if /i "%~1"=="-Project" (
    if "%~2"=="" (
        echo -Project requires a project directory or Project.heproject. 1>&2
        exit /b 2
    )
    set "HE_PROJECT_INPUT=%~2"
) else (
    set "HE_PROJECT_INPUT=%~1"
)
if not defined HE_PROJECT_INPUT set "HE_PROJECT_INPUT=%REPO_ROOT%\Project"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\Tools\Agent\Package.ps1" -Project "%HE_PROJECT_INPUT%"
exit /b %ERRORLEVEL%
