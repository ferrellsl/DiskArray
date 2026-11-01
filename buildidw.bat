@echo off
rem Builds the three IDW demo variants into build16\.
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h
cd build16
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. -fo=idwdisk.obj ..\idwdemo.cpp
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. -dUSE_RAM -fo=idwram.obj ..\idwdemo.cpp
wpp -q -ml -bt=dos -fpi -ox -w3 -zq -I.. -dBIG -fo=idwbig.obj ..\idwdemo.cpp
wlink option quiet system dos option stack=16k name idwdisk.exe file idwdisk.obj,dskarray.obj
wlink option quiet system dos option stack=16k name idwram.exe file idwram.obj,dskarray.obj
wlink option quiet system dos option stack=16k name idwbig.exe file idwbig.obj,dskarray.obj
cd ..
