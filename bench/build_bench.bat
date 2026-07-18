@echo off
rem Build bench.exe (native-engine benchmark prototype). Not a shipping artifact.
setlocal

where cl >nul 2>nul
if not errorlevel 1 goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [bench] vswhere.exe not found. Install Visual Studio 2022.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [bench] VC++ x64 toolset not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

:compile
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE bench.cpp /Fe:bench.exe
if errorlevel 1 exit /b 1
del /q bench.obj 2>nul
echo [bench] OK -^> bench\bench.exe
endlocal
