@echo off
rem Builds the bucketed gridder into build16\.
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h
cd build16
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. ..\idwb.cpp
if errorlevel 1 goto end
wlink option quiet system dos option stack=16k name idwb.exe file idwb.obj,dskarray.obj
:end
cd ..
