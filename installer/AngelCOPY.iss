; ===========================================================================
;  AngelCOPY installer  (Inno Setup 6)
;  Build the binaries first with build.bat, then compile this with ISCC.exe.
; ===========================================================================

#define AppName    "AngelCOPY"
#define AppVersion "1.5.0"

[Setup]
AppId={{7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D00}
AppName={#AppName}
AppVersion={#AppVersion}
SetupIconFile=..\assets\AngelCOPY.ico
AppPublisher=Valentin Klein
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
; Context-menu comparison panels on the task page (never installed):
; per language, per panel (std/cls), per selection state (sel/off).
Source: "taskcmp-de-std-sel.bmp"; Flags: dontcopy
Source: "taskcmp-de-std-off.bmp"; Flags: dontcopy
Source: "taskcmp-de-cls-sel.bmp"; Flags: dontcopy
Source: "taskcmp-de-cls-off.bmp"; Flags: dontcopy
Source: "taskcmp-en-std-sel.bmp"; Flags: dontcopy
Source: "taskcmp-en-std-off.bmp"; Flags: dontcopy
Source: "taskcmp-en-cls-sel.bmp"; Flags: dontcopy
Source: "taskcmp-en-cls-off.bmp"; Flags: dontcopy

[Icons]
Name: "{group}\AngelCOPY Anleitung"; Filename: "{app}\AngelCOPY Anleitung.txt"; Languages: de
Name: "{group}\AngelCOPY Guide"; Filename: "{app}\AngelCOPY Guide.txt"; Languages: en

[CustomMessages]
en.ExplorerRestartInfo=Windows Explorer will be closed during the installation.%n%nYour %1 open Explorer window(s) will be reopened automatically when the installation finishes.
de.ExplorerRestartInfo=Der Windows Explorer wird während der Installation geschlossen.%n%nIhre %1 offenen Explorer-Fenster werden nach Abschluss der Installation automatisch wieder geöffnet.

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
  // How many Explorer windows were open when setup killed Explorer; their
  // folder paths sit in a file in the ORIGINAL USER's %TEMP%, written and
  // read back by PowerShell running as that user. Two hard-won rules:
  // the elevated setup process cannot see the medium-integrity Explorer's
  // window list at all (first version enumerated from Pascal and saved
  // nothing), and the file must NOT live in the setup's elevated {tmp}
  // (second version: the medium-integrity writer/reader pair did not
  // reliably see it there, and Inno deletes {tmp} on its own schedule).
  // $env:TEMP expands inside the user's own PowerShell on both ends, so
  // both sides see the same path by construction.
  SavedWindowCount: Integer;

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

// Comparison panels under the classic-menu checkbox: default Win11 menu
// (AngelCOPY hidden behind "Show more options") vs the classic full menu.
// The panels are CLICKABLE — clicking one selects that option (sets/clears
// the task checkbox) — and the selected panel carries an orange frame, so
// checkbox state and picture always agree. Fail-open on any error.
var
  CmpLeft, CmpRight: TBitmapImage;

function CmpLang(): String;
begin
  if ActiveLanguage = 'de' then Result := 'de' else Result := 'en';
end;

procedure UpdateCmpFrames;
var
  L, R: String;
begin
  if (CmpLeft = nil) or (CmpRight = nil) then Exit;
  if WizardIsTaskSelected('classicmenu') then
  begin
    L := 'taskcmp-' + CmpLang() + '-std-off.bmp';
    R := 'taskcmp-' + CmpLang() + '-cls-sel.bmp';
  end else begin
    L := 'taskcmp-' + CmpLang() + '-std-sel.bmp';
    R := 'taskcmp-' + CmpLang() + '-cls-off.bmp';
  end;
  try
    CmpLeft.Bitmap.LoadFromFile(ExpandConstant('{tmp}\') + L);
    CmpRight.Bitmap.LoadFromFile(ExpandConstant('{tmp}\') + R);
  except
  end;
end;

procedure SetClassicTask(Enable: Boolean);
var
  I: Integer;
begin
  // Only one task exists; the group-description row has no checkbox and
  // ignores the write.
  for I := 0 to WizardForm.TasksList.Items.Count - 1 do
    WizardForm.TasksList.Checked[I] := Enable;
  UpdateCmpFrames;
end;

procedure CmpLeftClick(Sender: TObject);
begin
  SetClassicTask(False);
end;

procedure CmpRightClick(Sender: TObject);
begin
  SetClassicTask(True);
end;

procedure TasksListClickCheck(Sender: TObject);
begin
  UpdateCmpFrames;
end;

procedure InitializeWizard();
var
  Y: Integer;
begin
  try
    ExtractTemporaryFile('taskcmp-' + CmpLang() + '-std-sel.bmp');
    ExtractTemporaryFile('taskcmp-' + CmpLang() + '-std-off.bmp');
    ExtractTemporaryFile('taskcmp-' + CmpLang() + '-cls-sel.bmp');
    ExtractTemporaryFile('taskcmp-' + CmpLang() + '-cls-off.bmp');
    // The tasks list is a full-page opaque scroll box — anything placed
    // "under" the checkbox is painted over unless the list is shrunk first.
    WizardForm.TasksList.Height := ScaleY(56);
    WizardForm.TasksList.OnClickCheck := @TasksListClickCheck;
    Y := WizardForm.TasksList.Top + WizardForm.TasksList.Height + ScaleY(8);

    CmpLeft := TBitmapImage.Create(WizardForm);
    CmpLeft.Parent := WizardForm.SelectTasksPage;
    CmpLeft.Stretch := True;
    CmpLeft.Left := WizardForm.TasksList.Left;
    CmpLeft.Top := Y;
    CmpLeft.Width := ScaleX(212);
    CmpLeft.Height := ScaleY(140);
    CmpLeft.Cursor := crHand;
    CmpLeft.OnClick := @CmpLeftClick;

    CmpRight := TBitmapImage.Create(WizardForm);
    CmpRight.Parent := WizardForm.SelectTasksPage;
    CmpRight.Stretch := True;
    CmpRight.Left := WizardForm.TasksList.Left + ScaleX(224);
    CmpRight.Top := Y;
    CmpRight.Width := ScaleX(212);
    CmpRight.Height := ScaleY(140);
    CmpRight.Cursor := crHand;
    CmpRight.OnClick := @CmpRightClick;

    UpdateCmpFrames;
  except
    // no picture is better than a blocked setup
  end;
end;

// Task selection is finalized when the page shows. First visit: preselect
// what Windows is ACTUALLY set to right now — the classic-menu key existing
// means the user runs the classic menu, so the page must open with that
// panel framed instead of claiming the default. Later visits (Back button)
// keep whatever the user chose.
var
  CmpPreselected: Boolean;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectTasks then
  begin
    if not CmpPreselected then
    begin
      CmpPreselected := True;
      if ClassicPreexisting then SetClassicTask(True);
    end;
    UpdateCmpFrames;
  end;
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

// Remember every open Explorer window's folder BEFORE Explorer is killed, so
// the install doesn't cost the user their workspace. MUST run as the
// original (medium-integrity) user: Shell.Application.Windows is served by
// the user's Explorer, and the elevated setup process typically sees an
// EMPTY list. PowerShell writes the paths to the temp file and returns the
// count as its exit code. Virtual locations come back as ::{CLSID} paths,
// which explorer.exe reopens just as well. Strictly fail-open: any failure
// must never block the install.
procedure SaveOpenExplorerFolders;
var
  Cmd: String;
  ResultCode: Integer;
begin
  SavedWindowCount := 0;
  Cmd := '-NoProfile -NonInteractive -Command "' +
    '$f = Join-Path $env:TEMP ''AngelCOPY_windows.txt''; ' +
    '$p = @(); $s = New-Object -ComObject Shell.Application; ' +
    'foreach ($w in @($s.Windows())) { try { ' +
    'if ($w.FullName -match ''explorer\.exe'') { ' +
    '$x = $w.Document.Folder.Self.Path; if ($x) { $p += $x } } } catch {} }; ' +
    '$p | Set-Content -LiteralPath $f -Encoding Unicode; ' +
    'if (Test-Path -LiteralPath $f) { exit [Math]::Min($p.Count, 255) } ' +
    'else { exit 0 }"';
  if ExecAsOriginalUser('powershell.exe', Cmd, '', SW_HIDE,
                        ewWaitUntilTerminated, ResultCode) then
    SavedWindowCount := ResultCode;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  Msg: String;
begin
  SaveOpenExplorerFolders;
  // Tell the user what is about to happen to their windows — closing
  // Explorer unannounced looks like a crash. (Single line on purpose: a
  // continuation line starting with '[' is parsed as a section tag.)
  if SavedWindowCount > 0 then
  begin
    Msg := FmtMessage(CustomMessage('ExplorerRestartInfo'), [IntToStr(SavedWindowCount)]);
    MsgBox(Msg, mbInformation, MB_OK);
  end;
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

// Reopen the remembered folders once everything is done: the [Run] entries
// (which restarted Explorer) execute before the Finished page, ssDone fires
// after it — Explorer is back by then. ExecAsOriginalUser keeps the new
// windows at MEDIUM integrity (same reasoning as the [Run] flags above).
// PowerShell reads the same file it wrote (encoding stays its problem) and
// deletes it afterwards.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Cmd: String;
  ResultCode: Integer;
begin
  // The task page is the CONTROL for the classic-menu setting, both ways
  // (explicit user decision, Sep 2026): unchecking it removes the key even
  // when the user set it themselves long before this install — previously
  // deselecting silently did nothing. Runs before [Run]'s Explorer restart,
  // so the change takes effect immediately. The ownership rule still governs
  // UNINSTALL: only a key we created is deleted then (ClassicMenuIsOurs).
  if CurStep = ssPostInstall then
  begin
    if not WizardIsTaskSelected('classicmenu') then
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER,
        'Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}');
  end;

  if (CurStep = ssDone) and (SavedWindowCount > 0) then
  begin
    Cmd := '-NoProfile -NonInteractive -Command "' +
      '$f = Join-Path $env:TEMP ''AngelCOPY_windows.txt''; ' +
      'if (Test-Path -LiteralPath $f) { ' +
      'Get-Content -LiteralPath $f | ForEach-Object { ' +
      'if ($_.Trim()) { Start-Process -FilePath explorer.exe ' +
      '-ArgumentList (''\"'' + $_.Trim() + ''\"'') } }; ' +
      'Remove-Item -LiteralPath $f -Force }"';
    ExecAsOriginalUser('powershell.exe', Cmd, '', SW_HIDE,
                       ewWaitUntilTerminated, ResultCode);
  end;
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
