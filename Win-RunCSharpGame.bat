@echo off
setlocal
call "%~dp0hectl.bat" game open -Configuration RelWithDebInfo -StartupScene Assets/Scenes/MainGameCSharp.he %*
set "runExitCode=%ERRORLEVEL%"
if not "%runExitCode%"=="0" (
    echo C# game failed. Review the output above.
    pause >nul
)
exit /b %runExitCode%
