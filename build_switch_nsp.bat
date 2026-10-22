@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem OpenTricky for the Nintendo Switch -- a home-menu icon (.nsp) for the game,
rem made from your own keys and picture, on Windows.
rem   build_switch_nsp.bat "C:\path\to\prod.keys" "C:\path\to\cover.png"
rem Needs MSYS2 and Docker Desktop, as build_switch.bat -- see docs/switch.md.
setlocal
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
  echo MSYS2 not found in %MSYS2%.
  echo Install it from https://www.msys2.org/ or set MSYS2_ROOT to its folder.
  pause
  exit /b 1
)
if "%~2"=="" (
  echo usage: build_switch_nsp.bat "C:\path\to\prod.keys" "C:\path\to\cover.png"
  pause
  exit /b 2
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
set "MSYS2_PATH_TYPE=inherit"
cd /d "%~dp0"
"%MSYS2%\usr\bin\bash.exe" -lc "./build_switch_nsp.sh \"$(cygpath -u '%~f1')\" \"$(cygpath -u '%~f2')\""
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" echo Build failed (%ERR%). See the messages above.
pause
exit /b %ERR%
