@echo off
setlocal EnableExtensions

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\Dependencies\Install-SourceDependencies.ps1" %*
exit /b %ERRORLEVEL%
