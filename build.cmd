@echo off
rem Double-click to build Kantan Video Editor and start it.
rem The first run downloads the build tools (about 1.1 GB) into %USERPROFILE%\kantan-dev
rem (C:\kantan-dev if the user folder name has non-ASCII characters or spaces).
rem Details: README.md
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0native\build.ps1" -Run %*
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)
