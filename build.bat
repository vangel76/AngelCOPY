@echo off
rem ===========================================================================
rem  AngelCOPY build script  --  produces dist\AngelCopyShell.dll + Runner.exe
rem  Requires: Visual Studio 2022 with the "Desktop development with C++" (x64)
rem  workload. Run from a normal cmd prompt (this script finds vcvars itself).
rem ===========================================================================
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [AngelCOPY] vswhere.exe not found. Install Visual Studio 2022.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [AngelCOPY] VC++ x64 toolset not found. Add it via the VS Installer.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist dist mkdir dist
if not exist build mkdir build

rem /utf-8 is required: shared\Localize.cpp holds German strings, and without it
rem MSVC reads the sources in the local ANSI codepage and mangles the umlauts.
echo [AngelCOPY] Compiling AngelCopyRunner.exe ...
cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE /Fo:build\ ^
   AngelCopyRunner\main.cpp AngelCopyRunner\Robocopy.cpp ^
   AngelCopyRunner\NativeCopy.cpp ^
   AngelCopyRunner\ProgressUI.cpp AngelCopyRunner\ConflictUI.cpp ^
   AngelCopyRunner\ConfirmUI.cpp AngelCopyRunner\Delete.cpp ^
   AngelCopyRunner\VolumeLock.cpp ^
   shared\Localize.cpp shared\Theme.cpp ^
   /Fe:dist\AngelCopyRunner.exe ^
   /link /MANIFEST:EMBED Shlwapi.lib Comctl32.lib Gdi32.lib User32.lib Advapi32.lib Ole32.lib
if errorlevel 1 exit /b 1
rem /MANIFEST:EMBED is load-bearing: the ComCtl32 v6 dependency must live INSIDE
rem the exe. As an external .manifest it works only while that file sits next to
rem the exe -- the installer ships just the exe, so the dialog would silently
rem fall back to unthemed controls once installed.

echo [AngelCOPY] Compiling AngelCopyAgent.exe ...
cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE /Fo:build\ ^
   AngelCopyAgent\main.cpp AngelCopyShell\Common.cpp shared\Localize.cpp ^
   /Fe:dist\AngelCopyAgent.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   User32.lib Shell32.lib Shlwapi.lib Ole32.lib OleAut32.lib
if errorlevel 1 exit /b 1

echo [AngelCOPY] Compiling AngelCopyShell.dll ...
cl /nologo /std:c++17 /EHsc /W4 /O2 /LD /utf-8 /DUNICODE /D_UNICODE /Fo:build\ ^
   AngelCopyShell\dllmain.cpp AngelCopyShell\Common.cpp AngelCopyShell\Guids.cpp ^
   AngelCopyShell\DragDropHandler.cpp AngelCopyShell\ContextMenu.cpp ^
   AngelCopyShell\Register.cpp shared\Localize.cpp ^
   /Fe:dist\AngelCopyShell.dll ^
   /link /DEF:AngelCopyShell\AngelCopyShell.def ^
   Ole32.lib Shell32.lib Shlwapi.lib User32.lib Advapi32.lib
if errorlevel 1 exit /b 1

rem Keep dist\ to exactly the shipping artifacts: import lib / exports / external
rem manifest are link byproducts and must not be mistaken for deliverables.
del /q dist\*.exp dist\*.lib dist\*.manifest 2>nul

echo.
echo [AngelCOPY] Build OK -^> dist\AngelCopyShell.dll, dist\AngelCopyRunner.exe
endlocal
