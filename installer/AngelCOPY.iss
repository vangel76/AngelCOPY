; ===========================================================================
;  AngelCOPY installer  (Inno Setup 6)
;  Build the binaries first with build.bat, then compile this with ISCC.exe.
; ===========================================================================

#define AppName    "AngelCOPY"
#define AppVersion "1.0.0"

[Setup]
AppId={{7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D00}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=AngelCOPY
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
OutputDir=..\dist
OutputBaseFilename=AngelCOPY-Setup
Compression=lzma2
SolidCompression=yes
; Shell extensions register machine-wide (HKLM) -> requires elevation.
PrivilegesRequired=admin
; Explorer is 64-bit; install + register in 64-bit mode.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
WizardStyle=modern

[Files]
; The runner first (no registration), then the shell DLL with self-registration.
Source: "..\dist\AngelCopyRunner.exe"; DestDir: "{app}"; Flags: ignoreversion
; regserver -> Inno calls DllRegisterServer on install / DllUnregisterServer on
; uninstall. uninsrestartdelete -> if Explorer still holds the DLL, remove it on
; the next reboot.
Source: "..\dist\AngelCopyShell.dll"; DestDir: "{app}"; Flags: ignoreversion regserver uninsrestartdelete 64bit

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  // Explorer keeps AngelCopyShell.dll loaded, which locks the file. Upgrading
  // in place fails unless Explorer is closed BEFORE the files are copied — the
  // [Run] entry below brings it back afterwards.
  Exec(ExpandConstant('{cmd}'), '/c taskkill /f /im explorer.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;

[Run]
; Restart Explorer so the new handlers load immediately.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden; StatusMsg: "Restarting Windows Explorer..."

[UninstallRun]
; Restart Explorer on uninstall so it releases the DLL and reverts to the
; stock drop handler (restored by DllUnregisterServer).
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden; RunOnceId: "RestartExplorer"
