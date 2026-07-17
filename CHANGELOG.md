# Changelog

All notable changes to AngelCOPY are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/); this
project aims to follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Removed
- **The invisible drag & drop interception never worked and has been removed.**
  It was built on a false premise: that Explorer honours a DropHandler
  registered under `Directory\shellex\DropHandler`. It does not — the shell
  serves folder drops from its own internal `IDropTarget` and never asked our
  handler anything (proven by the new `tests\test_droptarget.cpp`). Stock
  Windows registers a DropHandler only for `exefile`, `lnkfile` and
  `CompressedFolder`, never for folders; and `{BB2E617C-...}`, which the code
  chained to as "the stock handler", does not exist on Windows at all.
- Uninstall no longer writes that non-existent CLSID into
  `Directory\shellex\DropHandler` as a bogus "restore". Installing or
  uninstalling now cleans those stale keys off machines that have them: the key
  is deleted (it never existed before AngelCOPY), unless some other product
  genuinely owned it, in which case the real value is restored.

### Added
- **Low-disk-space warning before a copy/move/mirror starts.** The pre-scan now
  also computes how much space the transfer will actually consume and compares
  it to the destination volume's free space; if it looks too small, a dialog
  says how much is needed, how much is free, and the shortfall, before anything
  is written.
  - Counts **overwrite growth, not full source size**: robocopy overwrites
    in-place (measured — a 2 GB source over a 500 MB destination uses ~1.5 GB,
    with no temporary sidecar), so re-copying an existing tree no longer
    false-alarms. Shrinking files count as zero, never as freed space.
  - Runs **after** the conflict prompt, because the chosen policy decides how
    much is written ("Skip existing" needs far less than "Replace all").
  - **Advisory, not a hard block**: compression, deduplication or quota can beat
    the estimate, so "Try anyway" is offered with Cancel as the default. If the
    free-space query fails (e.g. a network share that reports nothing), the copy
    proceeds — the warning never blocks on uncertainty.
- **Mirror ("Spiegeln here FAST").** One-way sync: the destination is made an
  exact copy of the source — files are copied/overwritten, and anything at the
  destination that is not in the source is permanently deleted. On the
  right-drag menu (next to Copy/Move here, only when a folder is dragged) and in
  the context menu next to Paste FAST (only when the clipboard holds a folder).
  - **Copy phase, then purge phase, in that order — and the purge is skipped
    entirely if the copy failed** (robocopy code >= 8). A failed copy must never
    lead to deleting the destination's only copy of something. A cancel between
    the phases leaves a superset of the source: always safe.
  - **Mandatory confirmation** listing what will be copied *and* what will be
    permanently deleted, Cancel as the default button — same contract as the
    delete prompt. Headless (`--console sync`) refuses to delete extras without
    `ANGELCOPY_CONFIRM_DELETE=1`.
  - The purge reuses the existing recursive deleter: junctions/symlinks at the
    destination are removed as links, never recursed into (verified end-to-end —
    the link's target survives untouched), and it bypasses the Recycle Bin.
  - Not robocopy `/MIR`: deletion there is unparallelizable and needs the same
    care as our deleter anyway, and doing the purge ourselves keeps the junction
    safety and the progress reporting identical to Delete FAST.
- **Throughput chart in the progress dialog**, built like Windows' own copy
  dialog: progress and speed share **one** control — a solid band grows
  left→right for percent, the speed curve is drawn over it. The progress bar is
  gone; below the chart are the familiar `Name:` / `Time remaining:` /
  `Items remaining:` lines. The X axis is **progress**, not time: each sample is
  stored in the bucket for the percentage reached at that moment, so the curve
  ends exactly at the progress position and the axis is fixed at 0..100% of the
  transfer — it never scrolls or compresses. Drawing costs nothing measurable:
  robocopy runs in a separate process and the dialog's UI thread is otherwise
  idle.
- **Light/dark theme following the Windows setting.** Dark title bar via
  `DwmSetWindowAttribute`, control backgrounds via `WM_CTLCOLOR*`. Push buttons
  needed owner-drawing: measured, a `BS_PUSHBUTTON` stays RGB(240,240,240) in
  dark mode even after `SetWindowTheme(L"DarkMode_Explorer")`, and the usual
  workaround (undocumented uxtheme ordinal 135) was rejected. `ANGELCOPY_THEME=
  light|dark` forces a theme for testing.
- **Localization (German / English)**, picked from the system UI language
  (`shared\Localize.*`, shared by the DLL and the runner). Menus and all three
  dialogs follow it. Requires `/utf-8` when compiling — without it MSVC reads the
  sources in the ANSI codepage and mangles the umlauts.
- Menu entries now read **"<verb> FAST (AngelCOPY)"** — verb first, matching the
  shell's own items ("Hierher kopieren"), which is what the eye scans. FAST also
  keeps our entry distinguishable from the native one right below it.
  mouse button onto a folder or drive → "Fast Copy here" / "Fast Move here".
  This is the extension point the shell actually honours for drag & drop — the
  same one 7-Zip and WinMerge use. A left drag stays with Explorer; intercepting
  it invisibly would require hooking `IFileOperation` inside explorer.exe.
  (Menu text is localized, see above.)
- The installer now closes Explorer *before* copying files, so upgrading over a
  running install can actually replace the loaded DLL.
- **Fast Delete** (context menu, permanent only). Measured ~3.2x faster than
  Explorer on 5000 files (580 ms vs 1836 ms).
  - **Not robocopy.** The popular `robocopy /MIR` "fast delete" trick was
    measured and rejected: `/MT:64` and `/MT:1` are identical (908 vs 919 ms —
    robocopy does not parallelize its purge), and a plain single-threaded delete
    beats it. Deletion is NTFS-metadata bound; threads buy ~20% at best. The
    entire win comes from skipping Explorer's per-item shell overhead.
  - **No Recycle Bin option**, deliberately: the bin requires
    `IFileOperation` — the same engine Explorer uses (2473 ms measured), so
    there is no speed to win there. Offering it would be a misleading option.
  - Mandatory confirmation dialog (count, size, "cannot be restored"), with
    Cancel as the default button so Enter/Esc/closing never deletes.
  - Junctions and symlinks are removed, never followed into (verified: the
    link's target survives intact).
  - Read-only files are cleared and deleted; long paths use the `\\?\` form, so
    trees Explorer refuses to delete still work.
  - `--console delete` refuses to run without `ANGELCOPY_CONFIRM_DELETE=1`,
    since a headless irreversible delete must not be a silent default.
- **Conflict dialog.** A pre-scan classifies every source file against the
  destination; if files would actually be overwritten, a prompt lists them and
  offers **Replace all** / **Only if newer** (`/XO`) / **Skip existing**
  (`/XC /XN /XO`) / **Cancel** before anything is written. Previously robocopy
  silently overwrote — including letting an *older* source overwrite a *newer*
  destination.
  - Identical files (same size + mtime within 2s) are not conflicts: robocopy
    skips them anyway, so re-copying an unchanged tree still prompts nothing.
  - The progress bar is scaled to the volume the chosen policy will actually
    copy (`ExpectedFor`), so it still reaches 100% when files are skipped.
- **Skip reporting.** When files are skipped — identical (already up to date) or
  excluded by the chosen conflict policy — the dialog says so ("Done — N files
  skipped", with a per-reason breakdown and byte counts) and **stays open with a
  Close button**, like the error case. It only auto-closes when nothing was
  skipped and nothing failed.
- Progress dialog now lists robocopy's error lines in a scrollable box (window
  grows to fit) instead of a generic "see the destination" message. Shown
  whenever robocopy reports failures, even if the copy otherwise continued.

### Removed
- **"Copy FAST" context-menu entry.** It only put the selection on the
  clipboard — byte-identical to what Ctrl+C already does (CF_HDROP +
  PreferredDropEffect) — so it offered no speed and only lengthened the menu.
  The workflow is native Ctrl+C (or Ctrl+X for a move) followed by
  **Paste FAST** on the target; the paste is where robocopy happens.

### Changed
- **Skipped files are now part of the picture — as green, not as speed.** When
  part of a transfer already exists at the destination, the bar previously
  covered only the bytes actually being copied: a half-existing tree raced
  through a tiny remainder at cache speed and the graph read absurdly fast.
  Now the bar spans the full source volume; stretches robocopy skipped
  (identical files, or files excluded by the chosen conflict policy) are
  painted **green** with no throughput curve over them, while copied stretches
  keep the blue band + curve. Speed and ETA are computed from copied bytes
  only — a skipped file moves no data. An all-skip re-copy shows a fully green
  bar at 0 B/s.
  - Requires robocopy's `/V`: skipped files are not listed otherwise. Measured
    ~0.17 ms per skipped file (linear; 12000 all-skip: 116 ms → 2155 ms), paid
    only by skips.
  - A skip line is textually identical to a copy line under `/NC` (and the
    class word is localized — "Gleich"/"Älter" on German), so lines are
    classified by path against the pre-scan's skip set instead.
  - Side fix: robocopy also lists files that exist only at the destination
    ("extras") in the same line shape, with destination paths — these inflated
    the live file counter before. They are filtered out now.
- Parseable robocopy output now uses `/NP`, and the read pipe uses a 256 KB
  buffer, so robocopy's copy threads are no longer throttled by output the UI
  has to drain. Fixes the transfer slowdown under the progress dialog.

### Fixed
- **A single large file showed 0% / 0 B/s for the entire copy.** robocopy's
  stdout is a pipe, so its CRT buffers in 4 KB blocks — one huge file emits only
  ~100 bytes of text, which never reaches us until the process exits (verified
  with an 8 GB file). Byte progress now comes from the robocopy process's own IO
  counters (`GetProcessIoCounters`), which are live regardless of buffering, so
  the bar, rate and ETA work for single large files. Polling the destination file
  size would not have worked: robocopy pre-allocates it to full size at once.
  File count and current file name still come from the parsed output.
- German said "1 Dateien" — the file noun in the completion line was hardcoded
  plural.
- Report box overlapped the Close button: the layout mixed window and client
  coordinates (the title bar and borders were never accounted for). Layout is
  now defined in client coordinates and the window size derived via
  `AdjustWindowRectEx`.
- ETA is computed from the overall average (bytes copied / time elapsed so far),
  not the sliding window — a short window reacts to every `/MT` burst and made
  the estimate jump around uselessly. It now counts down monotonically.
- Transfer rate is now measured over a sliding ~3s window, and the whole-transfer
  average (labelled "average") is shown once finished. Two earlier attempts were
  both wrong: a per-tick rate spiked to absurd figures ("4.8 GB/s") because
  robocopy prints a file's line when it *starts* the file and `/MT:64` announces
  many at once, so the byte counter advances in bursts; the running average since
  start then read roughly half of the true rate, because it included robocopy's
  startup and directory enumeration, seconds in which no bytes flow. Measured
  against real destination byte growth, the window now tracks actual throughput.
- Progress showed "0%" next to "Done" when every file was skipped (nothing to
  copy means complete, not zero).
- Error detection is locale-independent: it matches the `(0xNNNNNNNN)` Win32
  error-code marker instead of the word "ERROR", so errors are captured on
  localized Windows (e.g. German robocopy prints "FEHLER").

## [1.0.0] — 2026-07-15

Initial release.

### Added
- **AngelCopyRunner.exe** — worker process wrapping `robocopy /MT:64`
  (`/E /COPY:DAT /R:2 /W:2 /XJ`, `/MOVE` for moves). Runs in its own process so
  a hang or crash never destabilizes Explorer.
  - `PlanJobs`: directories become whole-tree jobs; loose files are grouped by
    parent directory into a single robocopy call.
  - `@listfile` input (UTF-16LE), consumed and deleted after reading, to dodge
    the command-line length limit.
  - `--console` flag for headless text mode (used by tests); GUI dialog is the
    default.
- **AngelCopyShell.dll** — x64 in-proc COM shell extension (raw COM, no ATL).
  - Drop handler (`IDropTarget`) registered for Directory, Directory\Background
    and Drive; intercepts plain file/folder copy+move and delegates everything
    else (links, non-file data, exe-drops) back to the stock Windows handler.
  - Classic context-menu handler (`IContextMenu`): **Fast Copy** on a selection,
    **Fast Paste** on a folder / folder background.
  - `DllRegisterServer`/`Unregister` that backs up and restores the original
    drop-handler CLSID.
- **Native progress dialog** — percentage bar, transfer speed, ETA, current
  file, and a Cancel button (cancel kills the running robocopy). Auto-closes on
  success; stays open on error/cancel with a Close button.
- **Installer** (`AngelCOPY.iss`, Inno Setup) — machine-wide install,
  self-registration, Explorer restart, clean uninstall that restores stock
  behavior. Plus `scripts\install-dev.bat` / `uninstall-dev.bat` for quick dev
  iteration.
- **build.bat** — one-command build of both artifacts.
- **tests\test_load.cpp** — COM smoke test that validates the DLL without
  touching the registry or Explorer.

### Fixed
- Progress dialog did not auto-close on success — the `WM_TIMER` handler
  returned for every timer and swallowed the auto-close timer, leaving the
  window open until closed manually. Timer handling now branches on timer id.

### Known limitations
- `robocopy /MT` parallelizes per-file, so a single large file sees no speedup;
  the win is on many-file transfers.
- **Move + identical destination file:** robocopy only deletes source files it
  actually copied, so a file already present identically at the destination is
  neither copied nor removed from the source — a "move" can leave part of the
  source tree behind. Adding `/IS` would fix it at the cost of re-copying
  identical data.
- On Windows 11 the Fast Copy / Fast Paste entries appear under *Show more
  options*, not the main context-menu flyout.
- x64 only. Unsigned — SmartScreen/Defender may warn on the installer.
