@echo off
rem Dev install: register dist\AngelCopyShell.dll and restart Explorer.
rem Run as Administrator (right-click -> Run as administrator).
setlocal
cd /d "%~dp0.."

net session >nul 2>&1
if errorlevel 1 (
    echo [AngelCOPY] Must run as Administrator.
    exit /b 1
)
if not exist "dist\AngelCopyShell.dll" (
    echo [AngelCOPY] dist\AngelCopyShell.dll missing. Run build.bat first.
    exit /b 1
)

echo [AngelCOPY] Registering shell extension ...
regsvr32 /s "%CD%\dist\AngelCopyShell.dll"
if errorlevel 1 ( echo [AngelCOPY] regsvr32 failed. & exit /b 1 )

echo [AngelCOPY] Restarting Explorer ...
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
echo [AngelCOPY] Installed. Runner is used from: %CD%\dist\AngelCopyRunner.exe
endlocal
