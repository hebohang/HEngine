@echo off
setlocal EnableExtensions

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\Agent\Generate-Projects.ps1" -EnableTracy
if errorlevel 1 goto :error

if /I not "%~1"=="--no-pause" pause
exit /b 0

:error
echo [Win-GenProjects-Tracy] Configure or build failed.
if /I not "%~1"=="--no-pause" pause
exit /b 1
