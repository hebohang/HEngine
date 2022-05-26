@echo off

call cmake -B build -G "Visual Studio 17 2022" -T v143
call cmake --build build --parallel 4
PAUSE