@echo off
setlocal EnableExtensions

call cmake -S . -B build -DPACK_GAME=OFF -DHE_BUILD_DEV_GAME=OFF -DHE_ENABLE_GM=ON -DCPP_CODE_GEN=OFF -DHE_ENABLE_TRACY=OFF -DHE_TRACY_ON_DEMAND=ON -G "Visual Studio 17 2022" -A x64 -T v143
if errorlevel 1 goto :error

call cmake --build build --config Debug
if errorlevel 1 goto :error

if /I not "%~1"=="--no-pause" pause
exit /b 0

:error
echo [Win-GenProjects] Configure or build failed.
if /I not "%~1"=="--no-pause" pause
exit /b 1
