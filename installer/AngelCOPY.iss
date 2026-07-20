; ===========================================================================
;  AngelCOPY installer  (Inno Setup 6)
;  Build the binaries first with build.bat, then compile this with ISCC.exe.
; ===========================================================================

#define AppName    "AngelCOPY"
#define AppVersion "1.2.0"

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
; Start-menu folder for the guide shortcut (the group page stays hidden).
DefaultGroupName={#AppName}

[Languages]
; Language also picks the right guide for the post-install page and the
; Start-menu shortcut below.
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoAfterFile: "Anleitung-en.txt"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"; InfoAfterFile: "Anleitung-de.txt"

[Files]
; The runner first (no registration), then the shell DLL with self-registration.
Source: "..\dist\AngelCopyRunner.exe"; DestDir: "{app}"; Flags: ignoreversion
; regserver -> Inno calls DllRegisterServer on install / DllUnregisterServer on
; uninstall. uninsrestartdelete -> if Explorer still holds the DLL, remove it on
; the next reboot.
Source: "..\dist\AngelCopyShell.dll"; DestDir: "{app}"; Flags: ignoreversion regserver uninsrestartdelete 64bit
; Ctrl+V tray agent (low-level keyboard hook, fail-open).
Source: "..\dist\AngelCopyAgent.exe"; DestDir: "{app}"; Flags: ignoreversion
; Quick guide (shown on the post-install page; Start-menu shortcut below).
; File and shortcut carry the product name — a shortcut named just
; "Anleitung" is unfindable when someone searches for "AngelCOPY".
Source: "Anleitung-de.txt"; DestDir: "{app}"; DestName: "AngelCOPY Anleitung.txt"; Flags: ignoreversion
Source: "Anleitung-en.txt"; DestDir: "{app}"; DestName: "AngelCOPY Guide.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\AngelCOPY Anleitung"; Filename: "{app}\AngelCOPY Anleitung.txt"; Languages: de
Name: "{group}\AngelCOPY Guide"; Filename: "{app}\AngelCOPY Guide.txt"; Languages: en

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

// The shell DLL is loaded into EVERY user's Explorer (HKLM registration) and
// the agent autostarts from HKLM\...\Run. If {app} were writable by ordinary
// users, any standard user could replace either binary and get code execution
// in every other user's session — a privilege escalation created purely by
// choosing a bad install directory. The default ({autopf}) is safe; this only
// catches a hand-edited path. `icacls` is used because Inno has no ACL API.
// Compared by SID, never by group NAME: icacls/Get-Acl print localized names
// ("VORDEFINIERT\Benutzer" on German, not "BUILTIN\Users"), so a name-based
// check silently misses on most locales. S-1-5-32-545 = Users,
// S-1-5-11 = Authenticated Users, S-1-1-0 = Everyone. Exit code 1 = writable.
function DirWritableByUsers(const Dir: String): Boolean;
var
  ResultCode: Integer;
  Cmd: String;
begin
  Result := False;
  // GetAccessRules with SecurityIdentifier returns SIDs directly — no
  // IdentityReference.Translate(), which throws on unresolvable accounts and
  // would abort the whole check. 0x116 = Write|AppendData|WriteAttributes bits.
  Cmd := '-NoProfile -NonInteractive -Command "' +
         'try { ' +
         '$r = (Get-Acl -LiteralPath ''' + Dir + ''').GetAccessRules($true,$true,' +
         '[System.Security.Principal.SecurityIdentifier]); ' +
         '$bad = $r | Where-Object { $_.AccessControlType -eq ''Allow'' ' +
         '-and ($_.FileSystemRights.value__ -band 0x116) -ne 0 ' +
         '-and @(''S-1-5-32-545'',''S-1-5-11'',''S-1-1-0'') -contains ' +
         '$_.IdentityReference.Value }; ' +
         'if ($bad) { exit 1 } else { exit 0 } } catch { exit 2 }"';
  if not Exec('powershell.exe', Cmd, '', SW_HIDE, ewWaitUntilTerminated,
              ResultCode) then
    Exit;                       // can't run the check -> don't block install
  Result := (ResultCode = 1);   // 2 = couldn't read the ACL -> don't block
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if DirExists(WizardDirValue) and DirWritableByUsers(WizardDirValue) then
    begin
      MsgBox('This folder is writable by ordinary users.' #13#10#13#10 +
             'AngelCOPY loads a DLL into every user''s Explorer and autostarts ' +
             'a background agent from it, so installing here would let any ' +
             'standard user replace those files and run code in other users'' ' +
             'sessions.' #13#10#13#10 +
             'Please choose a protected location such as ' +
             ExpandConstant('{autopf}') + '\AngelCOPY.',
             mbError, MB_OK);
      Result := False;
    end;
  end;
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
; runasoriginaluser is LOAD-BEARING on both entries. Setup elevates
; (PrivilegesRequired=admin), and [Run] entries inherit the setup token — so
; without it the agent ran at HIGH integrity for the rest of the session
; (measured: agent HIGH while Explorer stayed MEDIUM). Two consequences, both
; verified: every copy/delete the agent launches via Ctrl+V / Shift+Del
; inherited admin rights and bypassed file ACLs, and UIPI blocked the runner's
; WM_COPYDATA "done" balloon to the now-higher-integrity agent window
; (ACCESS_DENIED). Restarting Explorer elevated is the same class of mistake.
; Restart Explorer so the new handlers load immediately.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden runasoriginaluser; StatusMsg: "Restarting Windows Explorer..."
; Start the Ctrl+V agent now (autostart only covers the next logon).
Filename: "{app}\AngelCopyAgent.exe"; Flags: nowait runasoriginaluser

[UninstallRun]
; Stop the agent first — its exe is locked while it runs.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im AngelCopyAgent.exe"; \
    Flags: runhidden; RunOnceId: "KillAgent"
; Restart Explorer on uninstall so it releases the DLL and reverts to stock.
; NOTE: runasoriginaluser is not a valid flag in [UninstallRun], so this one
; restart does inherit the uninstaller's elevated token. Accepted: it is a
; one-shot during removal (the product is going away, and the next logon
; starts Explorer normally), unlike the [Run] agent launch which would have
; stayed elevated for the whole session every day.
Filename: "{cmd}"; Parameters: "/c taskkill /f /im explorer.exe & start explorer.exe"; \
    Flags: runhidden; RunOnceId: "RestartExplorer"
