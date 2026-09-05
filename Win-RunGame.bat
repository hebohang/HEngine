@echo off
setlocal

call "%~dp0hectl.bat" game open -Configuration RelWithDebInfo %*
set "runExitCode=%ERRORLEVEL%"
if not "%runExitCode%"=="0" (
    echo.
    echo Win-RunGame failed. Review the output above, then press any key to close.
    pause >nul
)
exit /b %runExitCode%
