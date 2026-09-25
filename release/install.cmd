@echo off
rem Double-click to install MOHAVR (runs install.ps1 without changing your PowerShell settings).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
