# CLAUDE.md — AngelCOPY

Guidance for working in this repo.

## What this is

A Windows shell extension that replaces Explorer's slow single-threaded copy
with `robocopy /MT:64`. Nothing is invisible — Windows offers no supported hook
for Ctrl+V or a left drag (see gotchas). Integration is a **right-drag** menu
(Copy/Move here FAST) plus right-click **Copy / Paste / Delete FAST**. Native
dialogs show progress, conflicts and confirmation. Personal-use, unsigned, x64.

## Build & test

```bat
build.bat                                             REM -> dist\*.dll, *.exe
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\AngelCOPY.iss   REM -> dist\AngelCOPY-Setup.exe
```

- Toolchain: Visual Studio 2022, *Desktop development with C++* (x64). `build.bat`
  finds vcvars via `vswhere` itself — run it from a plain prompt.
- **Runner tests (headless):** `dist\AngelCopyRunner.exe --console copy <dest> <src...>`
  — the `--console` flag skips the GUI so exit codes are checkable in scripts.
  Without it the runner shows the progress dialog.
- **COM smoke test (no registry/Explorer):** compile & run `tests\test_load.cpp`
  against `dist\AngelCopyShell.dll` — loads the DLL, exercises both class
  objects and every interface. Use this to validate the DLL without registering.
- **Conflict / mirror unit tests:** `tests\test_conflict.cpp` (policy → flags,
  scan classification, skip sets, real robocopy outcomes) and `tests\test_sync.cpp`
  (`ScanExtras`: lonely dest entries, type mismatches, loose-file jobs never
  purge). Compile each with `Robocopy.cpp` + `Shlwapi.lib`, run, expect ALL PASS.
  Junction safety for mirror is covered by running `--console sync` against a
  tree whose destination holds a junction to a PRECIOUS folder — the link goes,
  the target survives.
- **Do NOT auto-register on the dev machine** — it kills and restarts the live
  Explorer. Let the user run `AngelCOPY-Setup.exe` or `scripts\install-dev.bat`.

## Architecture (three artifacts)

- `AngelCopyShell/` — x64 in-proc COM DLL, **raw COM (no ATL)** on purpose, so it
  builds regardless of which VS components are present. Kept thin: it only
  decides copy/move and launches the runner.
  - `DragDropHandler.cpp` — `IShellExtInit` + `IContextMenu`, registered under
    `Directory\shellex\DragDropHandlers` and `Drive\...`. Fires on a **right**-drag:
    the shell hands us `pidlFolder` = drop target and `pdtobj` = dragged items,
    and we add "Copy/Move here FAST" (localized).
  - `ContextMenu.cpp` — classic `IContextMenu` + `IShellExtInit` (Paste FAST /
    Delete FAST). Classic (not `IExplorerCommand`) on purpose. There is
    deliberately NO "Copy FAST": it only filled the clipboard — byte-identical
    to Ctrl+C — and the speed lives entirely in the paste. Removed July 2026;
    don't bring it back. Native Ctrl+C/X + Paste FAST is the workflow (Ctrl+X
    makes it a robocopy move via the clipboard's PreferredDropEffect).
  - `Register.cpp` — `DllRegisterServer`/`Unregister`: the CLSIDs plus the
    `DragDropHandlers` / `ContextMenuHandlers` keys. Also `CleanupLegacyDropHandler`,
    which strips the dead DropHandler keys (and the bogus `{BB2E617C-...}`
    "restore" value) off machines that ran AngelCOPY <= 1.0.
  - `Common.cpp` — CF_HDROP/clipboard helpers, `LaunchRunner` (writes a UTF-16LE
    temp list, spawns runner with `CREATE_NO_WINDOW`).
  - `Guids.cpp`, `dllmain.cpp` (factory + exports), `AngelCopyShell.def`.
- `AngelCopyRunner/` — x64 console-subsystem exe. The worker; separate process
  so a hang/crash never destabilizes Explorer.
  - `Robocopy.cpp` — `PlanJobs` (dirs → whole-tree jobs; loose files grouped by
    parent dir), `BuildRobocopyArgs` (`parseable` toggles the machine-readable
    flag set; `Conflict` policy adds `/XO` etc.), `ScanJobs` (classifies every
    file vs. the destination: lonely/same/newer/older), `ExpectedFor`, console
    `RunJobs`.
  - `ProgressUI.cpp` — native Win32 dialog + worker thread that spawns robocopy
    with a redirected pipe and parses output.
  - `ConflictUI.cpp` — pre-transfer conflict prompt (Replace / Only if newer /
    Skip / Cancel). `ConfirmUI.cpp` — the mandatory delete confirmation.
  - `Delete.cpp` — own recursive deleter (no robocopy; see gotchas).
  - **Mirror ("Spiegeln"):** `sync` op = copy phase (Replace policy) then purge
    phase (delete destination entries not in the source). `ScanExtras`
    (Robocopy.cpp) finds the extras — top-level only, recurses solely where both
    sides are real directories, returns reparse points as link targets it never
    enters. `AskSyncConfirm` (ConfirmUI.cpp) shows copy + delete counts with a
    list, Cancel default. `RunSyncWithUI` runs both phases in one dialog and
    flips `Shared::phaseDelete` between them (the heading switches to "Deleting",
    byte progress stays on the copy volume — deletion is metadata work). Load-
    bearing order: **copy first, purge second, and never purge if the copy
    returned >= 8** — a failed copy must not delete the destination's only copy
    of anything. A cancel between phases leaves a superset of the source: safe.
  - `main.cpp` — arg parse, `--console` vs GUI dispatch, scan → prompt → run.
- `shared/Localize.*` — all user-visible strings, compiled into BOTH binaries.
- `installer/AngelCOPY.iss` — Inno Setup. DLL entry has `regserver` (Inno calls
  Dll(Un)RegisterServer) + `uninsrestartdelete`; restarts Explorer on
  install/uninstall.

## Load-bearing gotchas

- **A LEFT drag onto a folder CANNOT be intercepted by any supported means.**
  `Directory\shellex\DropHandler` is *not* consulted for folders — the shell
  serves folder drops from its own internal `IDropTarget`. Proven by
  `tests\test_droptarget.cpp`, which asks the shell for a folder's drop target
  exactly the way Explorer does and gets back an object that isn't ours.
  Stock Windows registers a DropHandler only for `exefile`, `lnkfile` and
  `CompressedFolder` — never for `Directory`/`Folder`. AngelCOPY 1.0 shipped
  such a handler anyway (based on a wrong assumption that it was "how TeraCopy
  does it") and it never ran once. Do not resurrect it. The only extension point
  the shell honours for drag & drop is `shellex\DragDropHandlers` (right-drag);
  truly invisible left-drag would require hooking `IFileOperation` inside
  explorer.exe — the same fragile/AV-flagged class of hack rejected for Ctrl+V.
- **Ctrl+V stays with Windows — decided, don't re-propose.** The only
  non-injection route (a WH_KEYBOARD_LL background agent, PowerToys-style, with
  fail-open pass-through) was designed and offered in July 2026; the user chose
  not to build it. Paste FAST in the context menu is the paste integration.
- **Never invent a CLSID.** `{BB2E617C-...}` was used as "the stock drop handler"
  and does not exist on Windows at all. It was also written into the registry on
  uninstall as a "restore", i.e. pure junk. If a handler key did not exist before
  us, uninstall must DELETE it, not write a guessed value back.

- **robocopy `/MT` parallelizes per-file, never splits one file.** 64 threads
  only materialize with many files (verified: 400 files → 68 threads; 1 big file
  → 5). A single huge file therefore cannot be sped up with threads at all — its
  rate is simply the disk's (~500 MB/s cold here). Don't "fix" that.
- **Do NOT add HDD detection to throttle `/MT`.** It sounds obvious (spinning
  disks hate parallel streams) and the mechanism exists —
  `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceSeekPenaltyProperty`. It was
  rejected because **this machine has no HDD** (three SSDs: 2×NVMe + 1×SATA), so
  the *benefit* cannot be measured at all — only the detection could. Shipping an
  unmeasured heuristic is exactly what produced the dead DropHandler and the
  `/MIR` delete myth. A false positive (SSD behind a controller reporting a seek
  penalty) would throttle a fast array for nothing. If an HDD ever shows up:
  measure `/MT:64` vs `/MT:8` vs `/MT:2` on it first, then decide.
- **`/J` (unbuffered I/O) is deliberately NOT used.** Measured on a cold 8 GB
  file: 491 MB/s buffered vs 550 MB/s with `/J` — ~12% from a single sample, and
  it hurts many-small-file jobs. The user judged that not worth the branch.
  Beware: the *first* such measurement showed 3165 MB/s buffered because the
  source was still in the page cache — a warm-cache run measures nothing.
- **Never use robocopy to delete.** Measured: `/MIR` empty-mirror with `/MT:64`
  (908 ms) == `/MT:1` (919 ms) — robocopy does not parallelize its purge phase,
  and plain `rm -rf` (792 ms) beats both. Deletion is NTFS-metadata bound;
  parallelism buys ~20% at best (8 workers) and gets worse past that. `Delete.cpp`
  is a plain recursive deleter on purpose. The ~3x win over Explorer is purely
  from skipping shell overhead (SHFileOperation 1836 ms vs ours 580 ms).
- **Delete never offers the Recycle Bin.** The bin requires `IFileOperation`,
  i.e. Explorer's own engine (2473 ms measured) — there is nothing to win, so
  offering it would be a lie. Delete FAST is permanent-only and always confirms.
- **The deleter must not follow reparse points.** Junctions/symlinks are removed
  with `RemoveDirectoryW`, never recursed into — recursing would delete the
  link target's contents (someone else's data). There is a test for this.
- **Byte progress comes from `GetProcessIoCounters`, NOT from robocopy's output.**
  robocopy's stdout is a pipe, so its CRT buffers in 4 KB blocks: one huge file
  emits ~100 bytes of text and we receive *nothing* until the process exits — the
  dialog sat at 0 B / 0 B/s for an entire 8 GB copy (measured). Polling the
  destination file size does not help: robocopy pre-allocates it to full size
  instantly. The process's own IO counters are live regardless (verified: 1959 →
  3990 → 5988 → 8183 MB on that same file). Counters are per-process, so each
  finished job's `WriteTransferCount` is banked into `ioCompleted` *before* the
  handle is closed, and the running job's is added on top.
  - `Shared::bytesFromIo` gates this: the robocopy path sets it (and then
    `FeedChunk` must NOT also add file-line sizes — double counting); the delete
    path has no child process and keeps reporting bytes directly.
  - File count and current file name still come from the parsed output, so a
    single big file shows "0/1" until the end. Bytes are the useful signal.
- **`%` lines are useless under `/MT`** — they interleave across threads and
  can't be attributed to a file. `/NP` suppresses them. Parseable flags:
  `/BYTES /FP /NC /NDL /NJH /NJS /NP`.
- **robocopy's default overwrite is destructive and silent** — it overwrites any
  file that differs, *including overwriting a newer destination with an older
  source*. Hence the conflict prompt (`ConflictUI`) and the `Conflict` policy →
  flag mapping: Replace = default, Only-if-newer = `/XO`, Skip = `/XC /XN /XO`.
  Verified end-to-end in `tests\test_conflict.cpp`.
- **Identical files are never conflicts.** robocopy skips same-size/same-mtime
  (2s tolerance) files, so counting them would prompt on every re-copy *and*
  break the bar.
- **Skipped files are visible in the bar (green), but never in the speed.** The
  bar spans expected + skipped volume; skipped stretches are painted green and
  carry no throughput curve. Speed and ETA divide by **copied** bytes only —
  letting skips into the rate is exactly what made the graph "sehr schnell" on
  half-existing destinations. Mechanics:
  - `/V` is required in the parseable flag set — without it robocopy does not
    list skipped files at all. Cost: ~0.17 ms per skipped file, linear
    (measured: 4000 all-skip 80→880 ms, 12000: 116→2155 ms). Only skips pay it.
  - With `/NC` a skipped line is **textually identical** to a copy line (the
    class word — "Gleich"/"Älter" — is localized anyway, don't parse it). The
    only discriminator is the path: `SkipSetFor(scan, policy)` holds the
    lowercased source paths robocopy will list but not copy, and it must mirror
    `SkippedFor` exactly (under Replace, newer/older files ARE copied and must
    not be in the set).
  - robocopy also lists **"extra" files** (present only at the destination) as
    the same line shape — with their *destination* path. Filter lines whose
    path is under any job's dstDir, or every extra inflates the file counter
    (this bug existed before /V; extras print regardless).
  - A transfer that finishes within the first 150 ms sample interval (all-skip
    re-copy) would never write a chart bucket — completion flushes the pending
    sample unconditionally, and the first sample fills from bucket 0.
- **Errors are localized.** Detect the language-neutral `(0xNNNNNNNN)` marker,
  never the word "ERROR" — German robocopy prints "FEHLER" (this machine).
- **WM_TIMER must branch on timer id.** id 1 = refresh, id 2 = auto-close. A
  single `case WM_TIMER` that returns unconditionally swallows the auto-close and
  the dialog hangs open (this bug already happened once — don't reintroduce it).
- **The speed figure has two failure modes; both were hit.** robocopy prints a
  file's line when it *starts* the file and `/MT:64` announces many at once, so
  `doneBytes` advances in bursts: a per-tick rate spikes to nonsense ("4.8
  GB/s"). But the average since start reads ~half the real rate, because it
  includes robocopy's startup and directory enumeration where no bytes flow.
  Correct answer: a sliding ~3s window while copying, whole-transfer average
  (labelled) once done. Validate against real destination byte growth, not
  Task Manager — TM counts read+write, so a same-disk copy shows ~2x there.
- **Speed and ETA use different figures on purpose.** Speed = sliding window
  (what is happening now). ETA = overall average (`done / elapsed`), because the
  window makes the estimate jump on every burst. Don't "unify" them.
- **`ProgressUI` layout is in CLIENT coordinates**, window size comes from
  `AdjustWindowRectEx`. Mixing the two silently hid the Close button behind the
  report box (caption + borders are ~39px).
- **The dialog only auto-closes on a clean, complete run.** Anything skipped
  (`SkipInfo::any()`) or any error keeps it open with a Close button, so the user
  sees what happened. When everything is skipped `total == 0` — that must render
  as 100%, not 0%.
- **Win11 context menu:** classic `IContextMenu` shows under *Show more options*,
  not the main flyout. Main-menu placement needs an MSIX/`IExplorerCommand`
  package — deliberately out of scope for this unsigned personal build.
- Explorer keeps the DLL loaded → updating it requires an Explorer restart (the
  installer/dev scripts handle this).

## Progress dialog: chart + theme

- **The chart is progress AND throughput in one control** (like Windows' own copy
  dialog): a solid band grows left→right for percent, the speed curve is drawn
  over it. There is no `msctls_progress32` any more — don't add one back.
- **Double-buffer or it flickers.** `PaintChart` draws into a memory DC and blits
  once; `WM_ERASEBKGND` returns 1. At a 100 ms refresh, painting straight to the
  DC is unusable.
- **Drawing cannot slow the copy** — robocopy is a *separate process* and the
  dialog's UI thread is otherwise idle. Don't "optimise" the chart away.
- **X axis is PROGRESS, not time.** Each throughput sample goes into the bucket
  for the percentage reached at that moment (`RecordSample`), so the curve's
  right edge *is* the progress position and the axis is fixed at 0..100% of the
  transfer. Nothing scrolls, nothing compresses. This is what Explorer does — in
  its dialog the curve always ends exactly where the band ends. An earlier
  version used time on the X axis with dynamic compression; it looked plausible
  and was wrong.
- The curve is carried to the live `pct` at paint time: the newest sample can be
  one interval old, and without that the curve visibly trails the band it is
  supposed to be part of.
- **Skipped stretches are green band segments without a curve** (`skipLane`).
  Within one sample tick the copied/skipped split of the crossed buckets is
  proportional — /MT:64 interleaves both, exact ordering inside 150 ms is
  unknowable, don't try to reconstruct it.
- **Dark mode is not free.** Measured: a plain `BS_PUSHBUTTON` stays
  RGB(240,240,240) in dark mode *even after* `SetWindowTheme(L"DarkMode_Explorer")`.
  The common fix is the undocumented uxtheme ordinal 135 (`SetPreferredAppMode`)
  — deliberately not used. Dark buttons are `BS_OWNERDRAW` + `theme::DrawButton`.
  Side effect: `BS_OWNERDRAW` shares style bits with `BS_DEFPUSHBUTTON`, so a dark
  button cannot be the default one — Esc still cancels via `IsDialogMessage`, and
  Enter does nothing, which keeps the delete confirmation safe.
  Static/edit/listbox backgrounds need `WM_CTLCOLOR*`; the title bar needs
  `DwmSetWindowAttribute` (20, falling back to 19).
- **`ANGELCOPY_THEME=light|dark`** forces a theme, so the other one can be
  verified without touching the user's Windows setting.
- **Verify colours by screenshot + pixel, not by eye.** `PrintWindow` lies: it
  renders the frame unthemed, so the title bar looks classic even when it is
  correctly dark. Use `CopyFromScreen` on the real window instead.

## Localization

- All user-visible text lives in `shared\Localize.*` (one file, compiled into
  **both** the DLL and the runner). German if the system UI language is German,
  English otherwise.
- **`/utf-8` is mandatory** in both compile lines — the tables contain umlauts,
  and without it MSVC reads the sources in the ANSI codepage and mangles them.
- `static_assert` keeps both tables the same length as `enum S`. Add a string to
  the enum and one table only, and the build fails — as intended.
- **Every language must consume the same format arguments in the same order.**
  Word order may differ, argument order may not.
- **Grammar is not a noun swap.** Two traps already hit: German needs the dative
  plural after "in" ("in 2 **Ordnern**", hence `NounFoldersIn`), and a count
  changes the *verb* ("1 file differ**s**" / "1 Datei unterscheide**t** sich"),
  which is why `ConflictHead` is two separate strings rather than one with a
  swapped noun. When a count meets a verb, write both sentences.
- German labels are much longer than English ones — the conflict dialog's buttons
  had to move to two rows. Check layout in German, not just English.

## Conventions

- Namespaces: `angel::` in the DLL, `angelcopy::` in the runner.
- Fixed CLSIDs live in `Common.h` (string form) and `Guids.cpp` (binary). The
  base GUID is `7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D0x`.
- Keep the DLL dependency-free (raw COM, no ATL/.NET). Heavy/UI work belongs in
  the runner, never in the DLL (it runs inside Explorer).
