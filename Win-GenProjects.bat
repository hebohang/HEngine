@echo off

call cmake -B build
call cmake --build build --parallel 4
PAUSE