@echo off
setlocal
title QLIC WIC Setup

echo QLIC WIC Setup
echo Adds .qlic thumbnails and support to Explorer and WIC-aware apps.
echo Windows will ask for administrator permission once.
echo.

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-wic.ps1"
set "QLIC_SETUP_EXIT=%ERRORLEVEL%"
echo.

if not "%QLIC_SETUP_EXIT%"=="0" (
  echo QLIC WIC setup did not complete.
  echo No unverified installation is being reported.
  echo.
  pause
  exit /b %QLIC_SETUP_EXIT%
)

echo Setup is complete. You can now open .qlic images from Explorer.
echo.
pause
exit /b 0
