@echo off
setlocal
powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0Tools\Agent\ProjectManager.ps1" %*
exit /b %ERRORLEVEL%
