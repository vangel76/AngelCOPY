# AngelCOPY

Replace Windows Explorer's slow, single-threaded copy with **`robocopy /MT:64`**
throughput — integrated so normal actions trigger it.

- **Right-drag** files onto a folder or drive → **Copy here FAST** / **Move here
  FAST**.
- **Copy / paste** via right-click **Copy FAST** / **Paste FAST**.
- A native **progress dialog** with a throughput chart — progress band and speed
  curve in one control, like Windows' own copy dialog — plus current file, time
  remaining and items remaining. Follows the light/dark system theme. Errors are
  listed in the dialog.
- A **conflict dialog** asks before overwriting anything (Replace all / Only if
  newer / Skip existing / Cancel).
- **Delete FAST** — right-click → permanent delete, ~3x faster than Explorer.
  Always asks first.
- Installs machine-wide; uninstalls cleanly and restores stock behavior.

## Why is nothing invisible? (Ctrl+V, left-drag)

Windows offers **no supported way** to replace Explorer's copy for either
Ctrl+V or a plain left-drag:

- **Ctrl+V** — Explorer handles paste internally; there is no shell extension
  point for it.
- **Left-drag onto a folder** — the shell serves folder drops from its own
  internal `IDropTarget`. A DropHandler registered under
  `Directory\shellex\DropHandler` is **never consulted** (`tests\test_droptarget.cpp`
  proves this by asking the shell the same question Explorer does). Stock Windows
  registers drop handlers only for `exefile`, `lnkfile` and `CompressedFolder`.

Intercepting either would mean hooking Explorer from the inside (a keyboard hook,
or patching `IFileOperation` in explorer.exe) — fragile, antivirus-flagged, and
liable to break with Windows updates. AngelCOPY deliberately doesn't.

What *is* supported, and what AngelCOPY uses:

| Action | Extension point |
| --- | --- |
| Right-drag → "Copy here FAST" | `shellex\DragDropHandlers` (as used by 7-Zip, WinMerge) |
| Right-click → Copy / Paste / Delete FAST | `shellex\ContextMenuHandlers` |

## Language

The UI follows the system UI language: **German** if Windows is German, English
otherwise. Menu entries read `<verb> FAST (AngelCOPY)` — verb first, like the
shell's own items, with FAST keeping them apart from the native entry next to
them. All strings live in `shared\Localize.*`; adding a language means adding one
table (`/utf-8` is required to build).

## Architecture

```
AngelCopyShell.dll   x64 in-proc COM  — Explorer integration (thin)
AngelCopyRunner.exe  x64              — spawns robocopy /MT:64, shows progress
AngelCOPY-Setup.exe  Inno Setup       — install / register / uninstall
```

The DLL stays thin: all real work runs in the separate `AngelCopyRunner.exe`, so
a hang or crash during a transfer never destabilizes Explorer.

**Drag&drop handler** (`AngelCopyShell/DragDropHandler.cpp`) is an
`IShellExtInit` + `IContextMenu` handler registered under
`Directory\shellex\DragDropHandlers` and `Drive\...`. On a right-drag the shell
hands it both halves of the operation — `pidlFolder` is the folder the items were
dropped **on**, `pdtobj` the dragged items — and it offers *Copy/Move here FAST*.

**Context menu** (`AngelCopyShell/ContextMenu.cpp`) is a classic
`IContextMenu` + `IShellExtInit` handler: *Copy FAST* on a selection puts the
items on the clipboard; *Paste FAST* on a folder / background reads the
clipboard and runs the transfer; *Delete FAST* prompts, then deletes.

**Runner** (`AngelCopyRunner/`) turns a request into robocopy jobs:
`robocopy "<src>" "<dst>" /MT:64 /E /COPY:DAT /R:2 /W:2 /XJ` for folders,
a file-filter form for loose files, `/MOVE` (or `/MOV`) for moves.

## Build

Requires **Visual Studio 2022** with the *Desktop development with C++* (x64)
workload. From a normal command prompt:

```bat
build.bat
```

Produces `dist\AngelCopyShell.dll` and `dist\AngelCopyRunner.exe`.

To build the installer (requires **Inno Setup 6**):

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\AngelCOPY.iss
```

Produces `dist\AngelCOPY-Setup.exe`.

## Install

**Installer (recommended):** run `dist\AngelCOPY-Setup.exe` (elevates, registers
the shell extension, restarts Explorer). Uninstall via *Apps & features* — it
unregisters, removes its keys, and restarts Explorer.

**Dev scripts (no installer):** after `build.bat`, right-click →
*Run as administrator*:

```
scripts\install-dev.bat      register dist DLL + restart Explorer
scripts\uninstall-dev.bat    unregister + restart Explorer
```

## Verify it works

1. **Runner alone (no install needed):**
   `dist\AngelCopyRunner.exe copy D:\dest C:\some\bigfolder` — confirm files land
   and a `robocopy.exe` runs with `/MT:64` (Task Manager / `wmic process`).
2. **Right-drag (after install):** drag a folder onto another folder with the
   **right** mouse button, release → the menu contains **Copy here FAST** /
   **Move here FAST** (German Windows: **Hierher kopieren FAST**). Pick one;
   Task Manager shows `robocopy.exe` working.
3. **Context menu:** right-click files → **Copy FAST**; right-click a folder's
   empty space → **Paste FAST**; right-click files → **Delete FAST**.
4. **COM smoke test (no registry/Explorer):**
   `tests\test_load.cpp` loads the DLL and checks every interface.
5. **Drop-target reality check:** `tests\test_droptarget.cpp` asks the shell for
   a folder's `IDropTarget` the way Explorer does. It documents *why* a left drag
   cannot be intercepted — the object returned is never ours.

## What happens when files already exist?

A pre-scan compares every source file with the destination. **Identical** files
(same size, mtime within 2s) are skipped by robocopy and are not treated as
conflicts. If files would really be overwritten, AngelCOPY asks first:

| Choice | robocopy flags | Effect |
| --- | --- | --- |
| **Replace all** | *(default)* | Overwrite every differing file |
| **Only if newer** | `/XO` | Never let an older source overwrite a newer destination |
| **Skip existing** | `/XC /XN /XO` | Only copy files not already there |
| **Cancel** | — | Nothing is touched |

Existing destination folders are **merged**, never replaced, and files that
exist only at the destination are always left alone (no purge/mirror).

## Delete FAST

Right-click → **Delete FAST (AngelCOPY)**. It always shows a confirmation with
the file count and size; Cancel is the default button.

**Permanent only — it does not use the Recycle Bin and cannot be undone.**

Why permanent-only, and why not robocopy? Both were measured on 5000 files:

| Method | Time |
| --- | --- |
| Explorer engine, permanent (`SHFileOperation`) | 1836–2962 ms |
| Explorer engine → Recycle Bin | 2473 ms |
| `robocopy /MIR /MT:64` (the "fast delete" trick) | 908 ms |
| `robocopy /MIR /MT:1` | 919 ms |
| **AngelCOPY Delete FAST** | **580 ms** |

`/MT:64` and `/MT:1` are the same: robocopy does not parallelize deletion, so
the famous trick buys nothing over a plain delete. Deletion is bound by NTFS
metadata, not throughput — threads gain ~20% at best. The ~3x win is simply from
not paying Explorer's per-item shell overhead. And the Recycle Bin can only be
driven through Explorer's own engine, so there is no speed to gain there — which
is why that option isn't offered.

Junctions and symlinks are removed, never followed into. Read-only files are
deleted. Long paths (beyond `MAX_PATH`) work.

## Limitations / notes

- **x64 only.** Modern Explorer is 64-bit; the handler must match.
- **Move onto an identical file:** robocopy only deletes source files it
  actually copied, so a file already present identically at the destination is
  neither copied nor deleted from the source — a move can leave part of the
  source behind.
- **Windows 11 context menu:** the classic menu (Copy/Paste FAST) appears under
  **"Show more options"** (or Shift+F10). Putting entries in the *main* Win11
  menu requires shipping an MSIX/`IExplorerCommand` package — out of scope for
  this personal-use, unsigned build.
- **Move semantics:** on a fast-path drop AngelCOPY reports `DROPEFFECT_COPY`
  and lets `robocopy /MOVE` delete the source itself — this prevents Explorer
  from deleting source files while robocopy is still reading them.
- **Unsigned:** SmartScreen/Defender may warn on `AngelCOPY-Setup.exe`. Expected
  for an unsigned personal build.
- **Updating the DLL:** Explorer keeps it loaded; the installer restarts Explorer
  so a new build takes effect (and schedules locked-file cleanup on reboot).

## Layout

```
AngelCopyRunner/     main.cpp, Robocopy.{h,cpp}      — the worker exe
AngelCopyShell/      dllmain, Common, Guids,          — the COM shell DLL
                     DragDropHandler, ContextMenu, Register, .def
installer/           AngelCOPY.iss                    — Inno Setup script
scripts/             install-dev.bat, uninstall-dev.bat
tests/               test_load.cpp                    — COM smoke test
build.bat                                             — compiles both artifacts
```
