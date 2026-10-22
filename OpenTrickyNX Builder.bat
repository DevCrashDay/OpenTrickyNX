@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem OpenTrickyNX Builder: double-click to set up the tools, build the game from
rem your own disc image and copy it to the SD card (docs/windows-guide.md).
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0port\tools\windows\builder.ps1"
