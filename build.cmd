@echo off
rem Double-click to build e-editor and start it.
rem The first run downloads the build tools (about 1.1 GB) into %USERPROFILE%\e-editor-dev
rem (C:\e-editor-dev if the user folder name has non-ASCII characters or spaces).
rem Details: README.md
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0native\build.ps1" -Run %*
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)
