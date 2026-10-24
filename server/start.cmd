@echo off
rem Double-click to start the AI server of Kantan Video Editor.
rem The first run downloads Node.js into server\.runtime (about 30 MB) and installs the libraries.
rem While this window is open, the server picks up updated files (src, .env, package.json) and restarts by itself.
rem Stop it with Ctrl+C or by closing this window. Details: server\README.md
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start.ps1" %*
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)
