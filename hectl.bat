@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\Agent\hectl.ps1" %*
exit /b %ERRORLEVEL%
