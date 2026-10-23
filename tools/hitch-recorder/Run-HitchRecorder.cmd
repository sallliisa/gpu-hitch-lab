@echo off
setlocal
cd /d "%~dp0"
fltmc >nul 2>&1
if errorlevel 1 (
    echo Administrator access is required to collect kernel ETW events.
    powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
echo GPU Hitch Lab - ETW Hitch Recorder
echo F11 marks a hitch without blocking CapFrameX. Ctrl+C stops recording.
"HitchRecorder.exe" %*
echo.
echo Recorder stopped. Review logshitch-recorder-*ecorder.log.
pause
