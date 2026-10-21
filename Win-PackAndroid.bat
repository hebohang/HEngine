@echo off
call "%~dp0hectl.bat" android build %*
exit /b %errorlevel%
