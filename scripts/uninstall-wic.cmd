@echo off
setlocal
title QLIC WIC Removal

echo QLIC WIC Removal
echo Removes .qlic thumbnails and WIC support from this computer.
echo Windows will ask for administrator permission once.
echo.

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall-wic.ps1"
set "QLIC_REMOVE_EXIT=%ERRORLEVEL%"
echo.

if not "%QLIC_REMOVE_EXIT%"=="0" (
  echo QLIC WIC removal did not complete.
  echo.
  pause
  exit /b %QLIC_REMOVE_EXIT%
)

echo QLIC WIC removal is complete.
echo.
pause
exit /b 0
