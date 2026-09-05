@echo off
setlocal EnableExtensions

call cmake -S . -B build_tracy -DPACK_GAME=OFF -DHE_BUILD_DEV_GAME=OFF -DHE_ENABLE_GM=ON -DCPP_CODE_GEN=OFF -DHE_ENABLE_TRACY=ON -DHE_TRACY_ON_DEMAND=ON -G "Visual Studio 17 2022" -A x64 -T v143
if errorlevel 1 goto :error

call cmake --build build_tracy --config Debug --target HEngineEditor
if errorlevel 1 goto :error

if /I not "%~1"=="--no-pause" pause
exit /b 0

:error
echo [Win-GenProjects-Tracy] Configure or build failed.
if /I not "%~1"=="--no-pause" pause
exit /b 1
