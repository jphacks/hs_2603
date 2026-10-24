@echo off
rem Double-click to create an administrator of the AI server (or to reset an administrator's password).
rem It asks for the e-mail address and shows a temporary password. Details: server\README.md
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\create-admin.ps1" %*
echo.
pause
