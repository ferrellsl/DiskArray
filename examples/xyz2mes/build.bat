@echo off
rem ---------------------------------------------------------------------------
rem  Builds XYZ2MES.EXE (16-bit DOS, large model) with Open Watcom, into build16.
rem
rem  It needs DOSGUI (github.com/ferrellsl/DOSGUI).  By default it's looked for
rem  in a clone next to this repository's folder:
rem     ...\DiskArray\examples\xyz2mes   (here)
rem     ...\DOSGUI\src
rem  or set DOSGUI to its src folder.  DiskArray is this repository.
rem  Open Watcom is looked for in C:\WATCOM unless WATCOM is set.
rem ---------------------------------------------------------------------------
setlocal
cd /d %~dp0
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if "%DISKARRAY%"=="" set DISKARRAY=%~dp0..\..
if "%DOSGUI%"=="" set DOSGUI=%~dp0..\..\..\DOSGUI\src
if not exist "%DOSGUI%\gui.h" (
  echo DOSGUI not found in %DOSGUI%: clone github.com/ferrellsl/DOSGUI or set DOSGUI
  exit /b 1
)
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h
if not exist build16 mkdir build16

rem  -obmiler is -ox without -s: stack checking stays on (an overflow stops
rem  with a message instead of quietly corrupting memory)
set CC=wcc %XFLAGS% -q -ml -bt=dos -0 -fpi -os -w2 -zq
set CPP=wpp -q -ml -bt=dos -0 -fpi -obmiler -w3 -zq
set GUI=gfx gport dispfunc base window button menu fields scr_bar perc_bar help filedlg

for %%f in (%GUI%) do (
  %CC% -fo=build16\%%f.obj "%DOSGUI%\%%f.c"
  if errorlevel 1 exit /b 1
)
%CPP% -I"%DISKARRAY%" -fo=build16\dskarray.obj "%DISKARRAY%\dskarray.cpp"
if errorlevel 1 exit /b 1
%CPP% -I"%DISKARRAY%" -fo=build16\engine.obj engine.cpp
if errorlevel 1 exit /b 1
%CC% -I"%DOSGUI%" -fo=build16\xyz2mes.obj xyz2mes.c
if errorlevel 1 exit /b 1

if exist build16\gui.lib del build16\gui.lib
for %%f in (%GUI%) do (
  wlib -q -b build16\gui.lib +build16\%%f.obj
  if errorlevel 1 exit /b 1
)
rem  32 KB of stack: the GUI's calls sit on top of the gridding code's
wlink option quiet system dos option stack=32768 name build16\xyz2mes.exe file build16\xyz2mes.obj,build16\engine.obj,build16\dskarray.obj library build16\gui.lib
if errorlevel 1 exit /b 1
echo built build16\xyz2mes.exe
