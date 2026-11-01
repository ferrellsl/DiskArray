@echo off
rem Builds with Open Watcom for 16-bit DOS, large model, 8087 instructions with
rem an emulator fallback. Output goes to build16\.
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h
if not exist build16 mkdir build16
cd build16
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. ..\dskarray.cpp
if errorlevel 1 goto end
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -dDA_CHECK -I.. ..\dafuzz.cpp
if errorlevel 1 goto end
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. ..\dabench.cpp
if errorlevel 1 goto end
wlink option quiet system dos option stack=16k name dafuzz.exe file dafuzz.obj,dskarray.obj
if errorlevel 1 goto end
wlink option quiet system dos option stack=16k name dabench.exe file dabench.obj,dskarray.obj
if errorlevel 1 goto end
echo built
:end
cd ..
