@echo off
rem Dev uninstall: unregister the shell extension (restores the stock drop
rem handler) and restart Explorer. Run as Administrator.
setlocal
cd /d "%~dp0.."

net session >nul 2>&1
if errorlevel 1 (
    echo [AngelCOPY] Must run as Administrator.
    exit /b 1
)

echo [AngelCOPY] Unregistering shell extension ...
regsvr32 /s /u "%CD%\dist\AngelCopyShell.dll"

echo [AngelCOPY] Restarting Explorer ...
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
echo [AngelCOPY] Uninstalled. Stock drop handler restored.
endlocal
