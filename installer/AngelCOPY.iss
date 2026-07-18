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
; The classic-menu task writes HKCU on purpose (it is a per-user Explorer
; setting) even though setup elevates. On a personal single-user machine the
; elevating user IS the target user, which is exactly the intent — acknowledge
; the compiler warning rather than let it nag.
UsedUserAreasWarning=no
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
; Ctrl+V tray agent (low-level keyboard hook, fail-open).
Source: "..\dist\AngelCopyAgent.exe"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
; Optional, DEFAULT OFF: restore the Windows 10 "classic" full context menu on
; Windows 11 so AngelCOPY's classic IContextMenu entries sit on the first level
; instead of under "Show more options". This is a global Explorer change (every
; menu, not just ours) — hence off by default and clearly labelled.
Name: "classicmenu"; Description: "Restore the classic (full) right-click menu in Windows 11"; \
    GroupDescription: "Windows 11 integration:"; Flags: unchecked

[Registry]
; Autostart the Ctrl+V agent for every user of this machine.
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "AngelCopyAgent"; \
    ValueData: """{app}\AngelCopyAgent.exe"""; Flags: uninsdeletevalue

; Classic-menu switch (HKCU, per-user). Created ONLY when the task is chosen
; AND the key did not already exist (Check below). uninsdeletekey then removes
; it on uninstall — but only because WE created it. If the user set this key
; themselves before installing, ClassicMenuIsOurs returns False, we never write
; it and never delete it: their setting is left untouched (same ownership rule
; as the legacy DropHandler cleanup). Empty default value under InprocServer32
; is the documented trick.
Root: HKCU; Subkey: "Software\Classes\CLSID\{{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}"; \
    Flags: uninsdeletekeyifempty; Tasks: classicmenu; Check: ClassicMenuIsOurs
Root: HKCU; Subkey: "Software\Classes\CLSID\{{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32"; \
    ValueType: string; ValueData: ""; \
    Flags: uninsdeletekey; Tasks: classicmenu; Check: ClassicMenuIsOurs

[Code]
var
  ClassicPreexisting: Boolean;

// Capture, ONCE before anything is written, whether the classic-menu key was
// already present. Everything downstream keys off this so we never claim
// ownership of a key the user set themselves.
function InitializeSetup(): Boolean;
begin
  ClassicPreexisting :=
    RegKeyExists(HKEY_CURRENT_USER,
      'Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32');
  Result := True;
end;

// True only when WE may create/own the classic-menu key: the task is chosen
// and the key did NOT exist before this install. Pre-existing -> hands off,
// both on install (don't duplicate) and uninstall (don't delete).
function ClassicMenuIsOurs(): Boolean;
begin
  Result := not ClassicPreexisting;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  // Explorer keeps AngelCopyShell.dll loaded, which locks the file. Upgrading
  // in place fails unless Explorer is closed BEFORE the files are copied — the
  // [Run] entry below brings it back afterwards. The agent locks its exe the
  // same way.
  Exec(ExpandConstant('{cmd}'), '/c taskkill /f /im AngelCopyAgent.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{cmd}'), '/c taskkill /f /im explorer.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;

[Run]
; Restart Explorer so the new handlers load immediately.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden; StatusMsg: "Restarting Windows Explorer..."
; Start the Ctrl+V agent now (autostart only covers the next logon).
Filename: "{app}\AngelCopyAgent.exe"; Flags: nowait

[UninstallRun]
; Stop the agent first — its exe is locked while it runs.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im AngelCopyAgent.exe"; \
    Flags: runhidden; RunOnceId: "KillAgent"
; Restart Explorer on uninstall so it releases the DLL and reverts to the
; stock drop handler (restored by DllUnregisterServer).
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden; RunOnceId: "RestartExplorer"
