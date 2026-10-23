@echo off
setlocal
cd /d "%~dp0"
fltmc >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator permissions for Windows ETW tracing...
    powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-Diagnostics.ps1"
echo.
echo Diagnostic run finished. Logs are saved in the logs folder.
pause
