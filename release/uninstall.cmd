@echo off
rem Double-click to remove MOHAVR (runs uninstall.ps1 without changing your PowerShell settings).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
pause
