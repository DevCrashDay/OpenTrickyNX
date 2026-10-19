@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem OpenTricky for the Nintendo Switch -- build "SSX Tricky.nro" from your own
rem SSX Tricky (USA) Xbox disc image, on Windows.
rem   build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
rem   (or drag the .iso onto this file)
rem Needs MSYS2 with the same packages as the Windows build (docs/building.md)
rem and Docker Desktop, running -- see docs/switch.md.
setlocal
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
  echo MSYS2 not found in %MSYS2%.
  echo Install it from https://www.msys2.org/ or set MSYS2_ROOT to its folder.
  pause
  exit /b 1
)
where docker >nul 2>nul
if errorlevel 1 (
  echo Docker not found. Install Docker Desktop from https://www.docker.com/products/docker-desktop/
  echo and start it: it provides the Switch compiler ^(devkitPro's devkitpro/devkita64 image^).
  pause
  exit /b 1
)
if "%~1"=="" (
  echo usage: build_switch.bat "C:\path\to\SSX Tricky (USA).iso"
  pause
  exit /b 2
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
rem Keep the Windows PATH inside MSYS2, so docker.exe is found there too.
set "MSYS2_PATH_TYPE=inherit"
cd /d "%~dp0"
"%MSYS2%\usr\bin\bash.exe" -lc "./build_switch.sh \"$(cygpath -u '%~f1')\""
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" echo Build failed (%ERR%). See the messages above.
pause
exit /b %ERR%
