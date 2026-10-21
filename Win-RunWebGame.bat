@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\Scripting\BuildSurvivorWeb.ps1
if errorlevel 1 exit /b 1
echo Keep this window open while playing. Close it to stop the preview server.
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\Build\Preview.ps1 -Project Project -Profile web -OpenBrowser
