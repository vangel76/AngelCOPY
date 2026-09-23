# CLAUDE.md — AngelCOPY

Guidance for working in this repo.

## What this is

A Windows shell extension that replaces Explorer's slow single-threaded copy
with a native Win32 copy engine (`NativeCopy.cpp`). The robocopy execution
path (the old A/B fallback, `ANGELCOPY_ENGINE=robocopy`) was REMOVED Sep 2026
once the engine was declared done; `bench\bench.cpp` still measures against
robocopy.exe directly, and the planning/scan layer keeps its historic file
name `Robocopy.cpp`. Nothing is invisible — Windows offers no supported hook
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
- **Conflict / mirror unit tests:** `tests\test_conflict.cpp` (scan
  classification, the PolicyCopies predicate, overwrite-growth space
  accounting) and `tests\test_sync.cpp`
  (`ScanExtras`: lonely dest entries, type mismatches, loose-file jobs never
  purge). Compile each with `Robocopy.cpp` + `Shlwapi.lib`, run, expect ALL PASS.
- **Native engine unit tests:** `tests\test_native.cpp` (policy matrix on real
  files, tree/empty-dir copy, rename moves + skip-policy leftovers, junction
  safety, big-file ring incl. unaligned tail, read-only overwrite, cancel,
  skip/byte accounting). Compile with `NativeCopy.cpp` + `Robocopy.cpp` +
  `Shlwapi.lib`, run, expect ALL PASS.
- **Engine benchmarks:** `bench\bench.cpp` (+ `bench\build_bench.bat`) —
  standalone robocopy-vs-native measurements (cold cache via
  FILE_FLAG_NO_BUFFERING eviction, flush-to-disk inside every timing). The
  numbers in the native-engine gotchas below come from it; re-run before
  changing any tuning constant.
  Junction safety for mirror is covered by running `--console sync` against a
  tree whose destination holds a junction to a PRECIOUS folder — the link goes,
  the target survives.
- **Volume-queue unit tests:** `tests\test_vlock.cpp` (compile with
  `VolumeLock.cpp`) — volume extraction, mutual exclusion on a shared volume,
  cancel/force short-circuits. Abandoned-mutex recovery (a killed holder must
  free the volume) needs two processes; verify with a helper that acquires
  `Local\AngelCopyVol_X` and `TerminateProcess`es itself, then a second that
  must still acquire.
- **Do NOT auto-register on the dev machine** — it kills and restarts the live
  Explorer. Let the user run `AngelCOPY-Setup.exe` or `scripts\install-dev.bat`.

## Architecture (four artifacts)

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
    makes it a move via the clipboard's PreferredDropEffect).
  - `Register.cpp` — `DllRegisterServer`/`Unregister`: the CLSIDs plus the
    `DragDropHandlers` / `ContextMenuHandlers` keys. Also `CleanupLegacyDropHandler`,
    which strips the dead DropHandler keys (and the bogus `{BB2E617C-...}`
    "restore" value) off machines that ran AngelCOPY <= 1.0.
  - `Common.cpp` — CF_HDROP/clipboard helpers, `LaunchRunner` (writes a UTF-16LE
    temp list, spawns runner with `CREATE_NO_WINDOW`).
  - `Guids.cpp`, `dllmain.cpp` (factory + exports), `AngelCopyShell.def`.
- `AngelCopyRunner/` — x64 console-subsystem exe. The worker; separate process
  so a hang/crash never destabilizes Explorer.
  - `NativeCopy.cpp` — **the copy engine** (July 2026). Small files:
    `CopyFileExW` thread pool, one thread per directory in 256-file chunks,
    16 threads. Big files (>= 32 MiB): unbuffered overlapped ring, QD8 x
    8 MiB over an IOCP. Moves: attempt `MoveFileExW` first (whole-tree rename
    when the destination is absent; per-file otherwise), copy+delete only on
    `ERROR_NOT_SAME_DEVICE`. Progress/skips/errors via `CopySink` callbacks.
    Semantics parity with the old robocopy flags: /E, /COPY:DAT, /R:2 /W:2,
    /XJ. `ANGELCOPY_THREADS` overrides the pool size for measurements.
  - `Robocopy.cpp` — historic name; today the PLANNING/SCAN layer only:
    `PlanJobs` (dirs → whole-tree jobs; loose files grouped by parent dir),
    `ScanJobs` (classifies every file vs. the destination:
    lonely/same/newer/older), `ClassifyFile` (public: the engine must decide
    exactly like the scan), `PolicyCopies`, `ExpectedFor`, exclusions, the
    carried-verdict store.
  - `ProgressUI.cpp` — native Win32 dialog + worker thread driving the engine
    via `CopySink` callbacks.
  - `ConflictUI.cpp` — pre-transfer conflict prompt (Replace / Only if newer /
    Skip / Cancel). `ConfirmUI.cpp` — the mandatory delete confirmation.
  - `Delete.cpp` — own recursive deleter (see gotchas). Also
    holds `ScanDelete` (files/dirs/bytes, feeds the delete prompt AND the
    properties dialog) on the `ScanWork`/`DrainScanWork` 8-worker queue.
  - `PropsUI.cpp` — fast properties dialog (`props` op; Alt+Enter /
    "Properties FAST"). Opens instantly, counts live via ScanDelete's
    ScanProgress atomics (100 ms timer). A "size on disk" button existed
    briefly and was removed on request (Sep 2026) — the user judged the
    on-disk figure pointless; don't bring it back unasked. "Windows
    properties…"
    invokes the native sheet (verb `properties` / `SHMultiFileProperties`);
    the sheet runs on a thread INSIDE the runner process, so `ShowProps`
    keeps the process alive until every visible window is gone — exiting
    earlier closes the sheet under the user's cursor. Single files are
    deliberately NOT intercepted anywhere (native sheet is instant and
    complete); the dialog rebuilds no tabs — Security/Sharing/Previous
    Versions stay one click away behind the button, a full replacement would
    be the Recycle-Bin lie again.
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
    - **Unreal preset (directory exclusions):** a `.uproject` in a whole-tree
      source is detected (`IsUnrealProject`); the GUI offers to skip
      `DerivedDataCache`/`Intermediate`/`Saved`/`Binaries` (`AskUnrealPreset`,
      checkbox default on, not persisted). `SetExcludedDirs` sets process-wide
      read-only state (Robocopy.cpp) read by EVERY walk — deliberately not
      threaded through signatures. **Load-bearing, same shape as the
      source-junction bug:** an excluded dir must be skipped by the copy walk
      (`ScanTree`, `WalkStream`) AND the purge walk (`FindExtras`) — if the
      purge saw it, it would delete the very cache the user kept, because the
      copy never wrote it so it reads as "not in source". The check goes BEFORE
      the recursion in all three. Console never excludes.
      **The whole-tree rename fast paths refuse when
      exclusions are active** (`TryQuickRenameMove` AND `TryRenameTree`): a
      rename would move the excluded caches along, silently ignoring the
      user's checkbox — the per-file path is the only one that can honor it.
      Regression: `tests\test_sync.cpp` (purge-safety, validated to
      fail without the FindExtras guard) + `tests\test_native.cpp` (copy walk).
    - **Same-folder copy ("<name> - Kopie"):** pasting an item into the folder it
    already lives in makes a renamed copy (Explorer behavior) instead of
    colliding with itself. `PlanJobs` detects `dst == src` (case-insensitive):
    a whole-tree self-copy renames `dstDir` (via `UniqueCopyName`, bumps to
    " (2)", " (3)" …); a loose-file self-copy becomes its own job with
    `RoboJob::dstNames[i]` set. A same-folder MOVE is dropped as a no-op (never
    self-destruct). `copyWord` ("Kopie"/"Copy") is passed INTO PlanJobs so
    Robocopy.cpp keeps no Localize dependency (the unit tests don't link it).
  - `main.cpp` — arg parse, `--console` vs GUI dispatch, scan → prompt → run.
- `AngelCopyAgent/` — x64 Windows-subsystem tray exe: the Ctrl+V interceptor
  (see the Ctrl+V gotcha below for the design laws). Compiles
  `AngelCopyShell/Common.cpp` (clipboard + LaunchRunner; `g_hModule` stays
  null -> ModuleDir() = exe dir) and `shared/Localize.cpp`.
- `shared/Localize.*` — all user-visible strings, compiled into ALL binaries.
- `shared/Util.h` — header-only helpers shared by ALL binaries:
  `acutil::ExtLongPath` (THE canonical `\\?\` prefixer — the per-file
  `Ext`/`ExtPath`/`ExtP` wrappers forward here; the copies had drifted once),
  `HumanBytes`, `LowerCopy`, `WinErrText`. Header-only on purpose: no
  build-script change, tests compiling single .cpp files keep working, the
  DLL gains no dependency.
- `AngelCopyRunner/BatchQueue.h` — the one producer→pool streaming batch
  queue (mutex/cv/deque of per-directory batches) used by the copy walk
  (`ChunkQueue`), the scan classification (`ScanQueue`) and the deleter
  (`DelQueue`) via `using` aliases. All three producers split batches at
  ~256 files — that split is what stops one flat directory from pinning a
  single worker (the scan was missing it until Sep 2026).
- `installer/AngelCOPY.iss` — Inno Setup. DLL entry has `regserver` (Inno calls
  Dll(Un)RegisterServer) + `uninsrestartdelete`; restarts Explorer on
  install/uninstall. Quick guide `installer/Anleitung-{de,en}.txt` (UTF-8 WITH
  BOM — BOM-less .txt is read as ANSI by Inno and the umlauts mangle) doubles
  as the post-install InfoAfter page and a Start-menu shortcut; per-language
  via the [Languages] entries. No guide links inside the dialogs (decided July
  2026): the progress dialog is transient and the confirm dialogs must be
  read, not clicked away toward a help file.

## Native engine gotchas (all measured — re-run bench\bench.cpp before "fixing")

- **Tuning constants are measurements, not guesses.** 16 pool threads: {8,16,
  32,64} swept, >16 collapses on NTFS same-directory create contention (64 thr:
  130 MB/s vs 16 thr sharded: 428 MB/s on 10k x 64 KiB). Ring QD8 x 8 MiB beat
  QD16 x 16 MiB. Numbers: small nested +33% vs robocopy /MT:64, flat parity,
  8 GiB +32%, same-volume move ~1500x (rename vs copy+delete).
- **Directory sharding is the small-file win, chunking is its safety net.** One
  thread per directory avoids create contention; without the 256-file chunk
  split a single flat directory degenerates to ONE thread (measured: 111 MB/s
  vs 201). Don't "simplify" either half away.
- **`CopyFileExW` stays the per-file primitive.** Three measured/structural
  reasons: Win11 kernel fast-path (manual ReadFile/WriteFile loop measured
  ~40% slower on small files), SMB server-side copy (a manual loop would drag
  every byte over the wire twice), and free attribute+mtime+progress+cancel
  handling. Don't hand-roll small-file I/O.
- **Buffered big-file timings lie without a flush.** The page cache absorbs
  gigabytes; a timing that stops when the call returns measures RAM (first
  bench: 4.5 GB/s "copy"). Every bench timing flushes inside the timed region;
  keep it that way.
- **Moves are attempt-rename-first, never path-compared.** `MoveFileExW`
  without `MOVEFILE_COPY_ALLOWED`; only `ERROR_NOT_SAME_DEVICE` falls back to
  copy+delete. The filesystem answers same-volume questions — subst/junction/
  UNC path heuristics don't. Whole-tree rename only when the destination dir
  is absent (that's what keeps it one metadata op).
- **The pre-transfer scan runs behind the "Preparing" window** (`RunScanWithUI`
  in ProgressUI.cpp): a 500k-file tree takes seconds to classify, and before
  this existed the user saw NOTHING until the scan finished. The window shows
  a live file counter (`ScanProgress`, threaded through ScanJobs / ScanExtras /
  ScanDelete) and only becomes visible after ~300 ms so small transfers never
  flash it. Cancel during the scan aborts with nothing touched.
- **The scan's cost is the DESTINATION stat, not the source walk — and it is
  skipped or parallelized** (measured, 10000 files vs SMB): destination folder
  absent → every file is provably Lonely, zero dest stats, scan 10.5 s → 0.01 s
  (`dstExists` threaded through `WalkForScan`, one dir-existence check replaces
  one stat per file; false only ever cascades downward). Destination present →
  stats fan out to `kScanThreads = 8` workers (`ScanQueue`, per-dir batches,
  per-worker `ScanResult` merged at the end): 10.5 s → ~2 s, 5x. Same
  latency-bound reasoning and thread count as the deleter — re-measure per
  target before changing. `WalkStream` has the matching engine-side shortcut:
  a dest dir it just CREATED (`dstFresh`, from CreateDirectoryEx not returning
  ALREADY_EXISTS) classifies everything as Lonely without stats. Keep scan and
  engine decisions identical or totals drift (existing gotcha below).
- **Engine-side classification is DEFERRED into the copy pool** (`Item::classify`
  + mtime): under `!dstFresh` the walk enqueues small files unclassified and the
  pool worker does the `ClassifyFile` dest stat and the skip-vs-copy decision.
  Inline classification on the walk thread made a skip-heavy re-mirror run at
  ONE thread's stat rate while all 16 workers idled (a 123k-file Unreal
  re-mirror crawled for minutes; deferred: 20k all-skip files in 0.16 s). Big
  files stay inline-classified — they must be routed to the ring by the walk.
- **ETA is max(byte projection, file projection).** Byte-only ETA read "--:--"
  through entire skip phases (skips DO cost time — one dest stat each — the
  "skips cost microseconds" assumption was wrong on skip-heavy runs). The file
  projection uses doneFiles (which counts skips); a single huge file (0/1
  files) still falls back to the byte estimate. Speed stays copied-bytes-only.
- **GUI moves try `TryQuickRenameMove` BEFORE any scan**: a whole-tree move
  whose destination doesn't exist can't conflict, so a same-volume drag of a
  500k-file folder completes instantly with no window at all (Explorer-style).
  Don't add a scan "for the progress totals" there — there is nothing to show.
- **The engine's walk STREAMS into the copy pool** (`WalkStream` + `ChunkQueue`):
  the pool copies the first directory while later ones are still being
  enumerated, and skips advance the green bar during the walk. The first
  version collected the whole plan before copying — on 500k files the dialog
  sat at 0% for the entire (second) walk. Do not reintroduce a collect-then-
  execute phase.
- **The scan's verdicts are CARRIED to the engine** (`SetCarriedClasses` /
  `ClassifyOrCarried`, Sep 2026): GUI native runs collect per-file classes
  during the scan (`SetCollectClasses`, lowercased src → FileClass) and the
  copy pool looks them up instead of re-statting every destination — the
  second stat round WAS the copy phase on a skip-heavy mirror over a slow
  target ("warm-cached re-classification is free" only holds locally). Rules:
  a lookup MISS always falls back to a real `ClassifyFile` (file appeared or
  changed since the scan — carried verdicts are an optimization, never the
  only truth); a map over `kMaxCarriedClasses` (400k, ~100 MB) is DISCARDED
  and the engine re-classifies streamed as before; fast-path Lonely files are
  not in the map (the engine's `dstFresh` shortcut covers them statlessly);
  console runs never collect. Regression:
  `tests\test_native.cpp` "carried scan verdicts" — adversarial: a wrong
  carried "Same" for a changed file must make the engine SKIP it, proving the
  carry is consumed rather than re-derived.
- **The engine's walk must mirror `ScanTree` exactly** (reparse points skipped,
  same classification via the shared `ClassifyFile`), or the progress totals
  and the conflict prompt drift from what actually happens.
- **On a SATA SSD the big-file race is over before it starts.** Measured July
  2026 on D: (Samsung 850 EVO 2TB, SATA, 95% full, same-disk copy): a 4 GiB
  cold-cache copy lands at 234–272 MB/s for EVERY engine — robocopy 248–264,
  our ring 266–272 (+3–7%). Same-disk copy splits the ~550 MB/s bus into
  read+write; nothing an engine does can move that ceiling, and Explorer sits
  at the same wall. The engine still wins where the bus isn't the limit: small
  files +20–25% (dir-sharded 426–467 vs robocopy 326–387 MB/s; first robocopy
  run after tree creation is a cold-metadata outlier, ignore it) and
  same-volume moves (rename, ~0.3 ms vs robocopy's 8.7 s copy+delete on 2 GiB).
  "Barely faster than Explorer on D:" for big files is expected physics, not a
  regression — the +32% big-file win needs NVMe headroom.
- **The big-file ring is LOCAL-only — any remote end routes to `CopyFileExW`.**
  Measured July 2026 against \\mp-fileserver (2 GiB): share→share via
  CopyFileExW ~0.2 s (SMB server-side copy offload — the bytes never cross the
  wire) vs the ring's 6–7 s (every byte over the wire TWICE, ~30x); share→
  local ring 4.9–5.1 s vs buffered 2.0–2.7 s (unbuffered reads bypass the
  redirector's read-ahead, ~2x); local→share the ring won by only ~6%.
  `IsRemotePath` (UNC prefix or `GetDriveType == DRIVE_REMOTE`, cached per
  drive letter) gates `CopyOneBig`. This is a MEASURED remote rule — the ban
  on unmeasured remote heuristics stands for everything else.
- **SMB small files & delete are latency-bound, not engine-bound.** Measured on
  the same share: 10000 x 64 KiB lands at 31–38 MB/s for every engine
  (robocopy ≈ interleaved ≈ dir-sharded — per-directory sharding buys nothing
  remotely, there is no local NTFS create-lock in the path). Upload parity
  holds end-to-end (native ≈ robocopy ~5.5 s for 4000 files; first-contact
  run can be ~2x, ignore it). Delete on the share: sequential ~400 files/s,
  8 threads ~1400 (3.5x), 16 threads ~1650 (+15–20% over 8 — latency keeps
  scaling where local NTFS stopped at 8). kDelThreads stays 8: the local
  measurement says 16 is sometimes worse there, and a per-path thread count is
  complexity the +15% doesn't buy. Same-share move = rename, ~5 ms (robocopy
  copies+deletes, 1.3–2 s).
- **No CPU/cache tuning (AMD X3D etc.).** Copying is I/O- and NTFS-metadata-
  bound; nothing revisits cache lines. Same unmeasurable-heuristic trap as the
  HDD detection below.

## Load-bearing gotchas

- **The volume queue serializes transfers per drive** (`VolumeLock.cpp`): one
  named mutex `Local\AngelCopyVol_<letter>` per drive, held for the whole
  transfer. A job locks every source+dest volume before copying, so two runs on
  the same slow disk don't thrash it. Load-bearing details, none optional:
  - **A mutex, NOT a lock file.** If a runner is killed mid-transfer (I did this
    to the user twice — see [[never-kill-running-transfers]]) or crashes, the OS
    hands the next waiter `WAIT_ABANDONED` and it proceeds. A lock file would
    wedge the queue forever after exactly the failure that is most likely here.
  - **Volumes are acquired in sorted order** (`VolumesForPaths` sorts) — the
    single global lock order is what makes it deadlock-free. Never acquire out
    of order.
  - **The lock lives in `RunUI`'s worker wrapper, on the engine thread**, so the
    "Waiting…" state shows in the dialog and Cancel / "Start anyway" stay live.
    A mirror holds it across BOTH phases (copy + purge) — the wrapper wraps the
    whole two-phase worker, don't split it.
  - `--console` is NOT queued (scripts manage their own concurrency);
    `ANGELCOPY_NO_QUEUE=1` disables it everywhere. The "Start anyway" button is
    one-shot (`forceStart`), never persisted.
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
- **Ctrl+V is intercepted by `AngelCopyAgent.exe`** (decision revised July 2026
  after initially declining). WH_KEYBOARD_LL tray agent, PowerToys-style.
  Design laws, all load-bearing:
  - **The hook callback does ONLY cheap user32 checks** (foreground class
    CabinetWClass/ExploreWClass, `IsClipboardFormatAvailable(CF_HDROP)`, focus
    class). COM folder resolution happens on the main thread AFTER the
    swallow. A slow callback gets the hook silently removed by Windows.
  - **The hook is RE-REGISTERED every 30 s** (`kRehookTimer`): even a cheap
    callback exceeds LowLevelHooksTimeout once in days of uptime (load spike,
    disk waking) and Windows removes the hook with no error and no API to
    detect it — the agent looks alive but intercepts nothing (happened live,
    Aug 2026). Unhook+rehook is microseconds; don't remove the timer, and
    branch WM_TIMER on the timer id (same trap as ProgressUI).
  - **Fail-open everywhere:** resolution failure (virtual folder, zip, This
    PC) REPLAYS Ctrl+V via SendInput; the hook ignores `LLKHF_INJECTED`
    events so the replay can't loop. Agent dead/absent -> native Ctrl+V.
  - **Focus in any *EDIT* class passes through** — address bar, search box,
    F2-rename must keep native TEXT paste.
  - **A cut is consumed:** after launching a move-paste the agent empties the
    clipboard (Explorer semantics — a second Ctrl+V must not re-move).
  - **Shift+Delete** is intercepted too -> runner `delete` (confirmation +
    parallel permanent delete) on the Explorer SELECTION. Same edit-focus
    passthrough; empty/virtual selection replays native Shift+Delete.
  - **Alt+Enter** is intercepted too -> runner `props` (fast properties).
    Arrives as WM_SYSKEYDOWN (Alt held). Empty selection = the CURRENT
    folder (Explorer behavior); a single FILE replays native Alt+Enter on
    purpose (nothing to count, the native sheet has all the tabs); virtual
    folders/failure replay native. Same edit-focus passthrough and
    key-repeat suppression (g_enterHeld).
  - Resolution via `GetActiveShellView(hwnd)` (IShellWindows -> match HWND ->
    IShellBrowser -> QueryActiveShellView). Folder: IFolderView ->
    IPersistFolder2 -> SIGDN_FILESYSPATH. Selection: IShellView::GetItemObject
    (SVGIO_SELECTION) -> CF_HDROP. `AngelCopyAgent.exe --test-resolve` writes
    every open window's folder AND selection to %TEMP%\acp_agent_test.txt —
    headless verification.
  - **The tray icon is re-added on the `TaskbarCreated` broadcast.** Explorer
    discards every tray icon when it rebuilds the taskbar — and the installer
    restarts Explorer right after starting the agent, so without the re-add a
    FRESH INSTALL showed no icon while the hooks worked (happened live, Sep
    2026). Any tray app needs this; don't remove it.
  - Installer: autostarts via HKLM Run key, starts it post-install, kills it
    pre-install/uninstall (the exe is locked while running).
- **Never invent a CLSID.** `{BB2E617C-...}` was used as "the stock drop handler"
  and does not exist on Windows at all. It was also written into the registry on
  uninstall as a "restore", i.e. pure junk. If a handler key did not exist before
  us, uninstall must DELETE it, not write a guessed value back.

- **A single huge file cannot be sped up with threads** — parallelism is
  per-file (verified back in the robocopy days: 1 big file materialized only
  5 of 64 threads); its rate is simply the disk's. Don't "fix" that.
- **Do NOT add HDD detection to throttle thread counts.** It sounds obvious
  (spinning disks hate parallel streams) and the mechanism exists —
  `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceSeekPenaltyProperty`. It was
  rejected because **this machine has no HDD** (three SSDs: 2×NVMe + 1×SATA), so
  the *benefit* cannot be measured at all — only the detection could. Shipping an
  unmeasured heuristic is exactly what produced the dead DropHandler and the
  `/MIR` delete myth. A false positive (SSD behind a controller reporting a seek
  penalty) would throttle a fast array for nothing. If an HDD ever shows up:
  measure the thread sweep on it first, then decide.
- **Deletion is its own engine, never a copy tool's side effect.** (Historic:
  robocopy's `/MIR` "fast delete" was measured a myth — it never parallelized
  its purge; plain `rd /s` beat it. The ~3x win over Explorer comes from
  skipping shell overhead.)
  - **`Delete.cpp` IS the dir-sharded 8-thread pool** (July 2026): streaming
    walk feeds one directory's files per 256-file chunk to 8 workers, real
    directories are removed bottom-up afterwards, reparse points are removed
    as links inline. **`ScanDelete` (the confirmation count) runs on the same
    8-worker directory queue** (Aug 2026) — the serial recursive count made
    the delete prompt take minutes on latency-bound targets; the count is pure
    enumeration (sizes come free with the find data), so directory-granular
    parallelism is the whole win. Reparse points are counted but never
    entered, same as the deleter. Bench scenario D's "sequential recursive (Delete.cpp
    today)" label is the OLD algorithm kept as the bench baseline, not the
    shipping code.
  - **How much delete parallelism buys is DISK-DEPENDENT — measure per disk.**
    An early NVMe measurement of naive (non-sharded) parallelism said "~20% at
    best"; dir-sharded sharding changed that. Measured July 2026 on D: (850
    EVO SATA, 95% full), 10000-file tree, scenario D: old sequential 6.5–8.2k
    files/s, dir-sharded 8 threads 22.5–30.5k files/s — **3–4x**; 16 threads
    add nothing over 8 (sometimes worse). Before touching kDelThreads, re-run
    scenario D on the target disk — do not extrapolate between disks.
- **Delete never offers the Recycle Bin.** The bin requires `IFileOperation`,
  i.e. Explorer's own engine (2473 ms measured) — there is nothing to win, so
  offering it would be a lie. Delete FAST is permanent-only and always confirms.
- **The deleter must not follow reparse points.** Junctions/symlinks are removed
  with `RemoveDirectoryW`, never recursed into — recursing would delete the
  link target's contents (someone else's data). There is a test for this.
- **The installer's Explorer-window restore runs entirely as the ORIGINAL
  user.** Two failures shipped before it worked (Sep 2026): the elevated
  setup process sees an EMPTY `Shell.Application.Windows` list (saved 0
  windows while the user had several open), and PowerShell 5.1's
  `Start-Process -ArgumentList` does NOT quote arguments with spaces —
  explorer.exe got split arguments and opened the default window instead of
  the saved folder. Hence: save AND restore via `ExecAsOriginalUser` +
  powershell, the path list in the user's own `$env:TEMP` (never the
  elevated `{tmp}`), and explicit `"` around each path. Also: a [Code]
  continuation line starting with `[` parses as a section tag.
- **Installer `[Run]` entries MUST carry `runasoriginaluser`.** Setup elevates
  (`PrivilegesRequired=admin`) and `[Run]` inherits that token — without the
  flag the agent ran at HIGH integrity all session (measured), so every Ctrl+V
  copy and Shift+Del delete it launched had admin rights, and UIPI blocked the
  runner's balloon to it. `[UninstallRun]` does not support the flag; that one
  restart stays elevated and is accepted (one-shot, product being removed).
- **Reparse-point checks are needed on BOTH sides in the mirror.** `FindExtras`
  once checked only the destination entry; a SOURCE-side junction was entered,
  and since the copy phase skips source junctions the purge compared the
  destination against the LINK TARGET and deleted the difference. Covered by
  `tests\test_sync.cpp` ("source-side junction is never traversed").
- **Every tree walk must use the `\\?\` prefix — enumeration AND stat, not just
  the I/O.** The copy/scan/mirror walkers once enumerated with a bare
  `srcDir + L"\\*"` while the copy used `ExtPath`; a path past MAX_PATH then made
  `FindFirstFile`/`GetFileAttributes` fail, so the scan silently dropped files
  (copy reported complete but short) and — worse — in a mirror an existing
  source read as "missing" and its destination twin was purged. `Ext`/`ExtPath`
  now wraps every `FindFirstFileExW` pattern and every attribute query in
  `ScanTree`, `FindExtras`, `ClassifyFile` and `WalkStream`. Never add a walk
  that stats an un-prefixed path. This also covers the *helpers*: `IsDirectory`
  (a long source folder read as a file → planned as a loose-file job → copied
  nothing), `Exists` (`UniqueCopyName` overwrote an existing "… - Kopie"), the
  loose-file stat in `RunNativeJobs`, and the rename probes. Regression:
  `tests\test_native.cpp` "Long paths" — note it needs a long SOURCE ROOT, not
  just a deep tree, to exercise `IsDirectory`.
- **Recursion is depth-capped (`kMaxDepth`/`kMaxWalkDepth`/`kMaxScanDepth` = 900)**
  in every walker. `\\?\` paths allow thousands of nesting levels; without the
  cap a crafted deep tree overflows the ~1 MB stack and crashes mid-operation.
  The mirror-extras walk stops WITHOUT emitting extras at the cap — an unscanned
  level must never have its destination entries deleted as "missing".
- **Every command-line argument to the runner is quoted with `QuoteArg`**
  (Common.cpp), which doubles a trailing run of backslashes. A drive-root target
  ("D:\") otherwise becomes `"D:\"`, whose `\"` is an escaped quote that merges
  the following @list argument in. Source paths still go through the temp list
  file, never the command line (no filename injection possible).
- **Free-space warning uses OVERWRITE GROWTH, not source size.** Overwrites
  happen in-place (measured: a 2 GB source over a 500 MB destination consumes
  ~1.5 GB — the dip equals the net, never the full 2 GB; no temp sidecar). So
  `NeededSpaceFor` = lonely bytes (full) + Σ max(0, srcSize − dstSize) over
  the classes the policy copies. Shrinking files add zero and are never
  counted as freed (copy ordering is unknown — never wrong in the dangerous
  direction). The naive "source size vs free" check would false-alarm on
  every re-copy of an existing tree; don't reintroduce it.
  - **Order is load-bearing: conflict prompt first, space check second.** The
    chosen policy decides how much is written ("Skip existing" needs far less
    than "Replace"), so the need isn't known until the policy is picked.
  - A move that falls through to copy+delete needs space exactly like a copy
    (the rename fast paths never reach the space check); no special-casing.
  - For mirror the purge frees space but runs AFTER the copy, so it can't offset
    the copy's need — the check uses the copy figure only.
  - Advisory, never blocking: compression/dedup/quota can beat the estimate, so
    the dialog offers "Try anyway" (Cancel default). A failed `GetDiskFreeSpace`
    ExW (network share reporting nothing) returns "ok" — fail-open, never block.
    Use `lpFreeBytesAvailableToCaller` (quota-aware), not the volume total.
- **A plain Replace overwrite is destructive and silent** — it overwrites any
  file that differs, *including overwriting a newer destination with an older
  source* (robocopy's old default, kept as ours). Hence the conflict prompt
  (`ConflictUI`) and the `PolicyCopies` mapping, verified in
  `tests\test_conflict.cpp` and test_native's on-disk policy matrix.
- **Identical files are never conflicts.** Same-size/same-mtime files (2s
  tolerance, FAT/network granularity) are skipped, so counting them would
  prompt on every re-copy *and* break the bar.
- **Skipped files are visible in the bar (green), but never in the speed.** The
  bar spans expected + skipped volume; skipped stretches are painted green and
  carry no throughput curve. Speed and ETA divide by **copied** bytes only —
  letting skips into the rate is exactly what made the graph "sehr schnell" on
  half-existing destinations. The engine reports skips exactly via `onSkip`.
  A transfer that finishes within the first 150 ms sample interval (all-skip
  re-copy) would never write a chart bucket — completion flushes the pending
  sample unconditionally, and the first sample fills from bucket 0.
- **Error lines carry the language-neutral `(0xNNNNNNNN)` marker**
  (`ReportError`); the message text is the OS's own localized string. Never
  match on the word "ERROR" — this machine is German.
- **WM_TIMER must branch on timer id.** id 1 = refresh, id 2 = auto-close. A
  single `case WM_TIMER` that returns unconditionally swallows the auto-close and
  the dialog hangs open (this bug already happened once — don't reintroduce it).
- **The speed figure has two failure modes; both were hit.** Byte progress
  arrives in bursts (many parallel workers), so a per-tick rate spikes to
  nonsense ("4.8 GB/s"); but the average since start reads too low because it
  includes enumeration where no bytes flow. Correct answer: a sliding ~3s
  window while copying, whole-transfer average (labelled) once done. Validate
  against real destination byte growth, not Task Manager — TM counts
  read+write, so a same-disk copy shows ~2x there.
- **The progress lock was never the bottleneck — don't shave it further.**
  Measured Sep 2026 (30k x 1 KiB GUI copy, 5+5 interleaved): all-counters-
  under-one-CS 6964 ms vs lock-free atomics 6957 ms. The atomics stayed
  (clearer ownership, currentFile throttled to 50 ms saves real string
  copies), but any future "optimize the progress lock" idea is already
  answered: the copy is I/O-bound.
- **Speed and ETA use different figures on purpose.** Speed = sliding window
  (what is happening now). ETA = overall average (`done / elapsed`), because the
  window makes the estimate jump on every burst. Don't "unify" them.
- **`ProgressUI` layout is in CLIENT coordinates**, window size comes from
  `AdjustWindowRectEx`. Mixing the two silently hid the Close button behind the
  report box (caption + borders are ~39px).
- **The dialog only auto-closes on a clean, complete run.** Anything skipped
  (`SkipInfo::any()`) or any error keeps it open with a Close button, so the user
  sees what happened. When everything is skipped `total == 0` — that must render
  as 100%, not 0%. The **"Keep window open when done" checkbox** additionally
  turns the clean-run auto-close into Done+Close; its state is global
  (`HKCU\Software\AngelCOPY\KeepOpenAfterDone`) and saved on every toggle, not
  on exit — a killed process must not lose the choice. The completion summary
  always ends with the wall-clock duration.
- **Win11 context menu:** classic `IContextMenu` shows under *Show more options*,
  not the main flyout. Main-menu placement needs an MSIX/`IExplorerCommand`
  package — deliberately out of scope for this unsigned personal build.
- Explorer keeps the DLL loaded → updating it requires an Explorer restart (the
  installer/dev scripts handle this).

## Taskbar progress + completion balloon

- **Colored taskbar-button progress** (`ITaskbarList3`, ProgressUI.cpp): the
  transfer dialog's taskbar button fills green (`TBPF_NORMAL` +
  `SetProgressValue`), turns red on error (`TBPF_ERROR`), clears on close. Only
  the main transfer dialog has it — the "Preparing" window is a tool-window
  with no taskbar button on purpose. Needs COM: `RunUI` does its own
  `CoInitializeEx`/`CoUninitialize` and links `Ole32.lib`.
- **"Done" balloon via the agent, NOT the runner.** On a clean run that took
  >= 3 s the runner sends `WM_COPYDATA` (dwData 1, body text) to the agent
  window (`FindWindowW(L"AngelCopyAgentWnd")`); the agent pops the balloon on
  its persistent tray icon. The runner deliberately grows no tray icon of its
  own — it exits right after completion, which would kill any balloon it
  owned. Agent absent -> no balloon (graceful). Under 3 s or cancelled -> none
  (no popup spam on quick copies).
- **Delete shows items/sec, not bytes/sec.** Deletion is NTFS-metadata bound;
  a byte rate reads "12 GB/s" one tick and 0 the next. `UiState::deleteMode`
  (pure delete) or `phaseDelete` (mirror purge) switches the live rate, ETA,
  the chart curve height AND the final summary to files/sec (`ChartSpeedItems`
  / `StatsDoneItems`). Everything else stays bytes.

## Progress dialog: chart + theme

- **The chart is progress AND throughput in one control** (like Windows' own copy
  dialog): a solid band grows left→right for percent, the speed curve is drawn
  over it. There is no `msctls_progress32` any more — don't add one back.
- **Double-buffer or it flickers.** `PaintChart` draws into a memory DC and blits
  once; `WM_ERASEBKGND` returns 1. At a 100 ms refresh, painting straight to the
  DC is unusable.
- **Drawing cannot slow the copy** — the engine runs on its own worker
  threads and the dialog's UI thread is otherwise idle. Don't "optimise" the
  chart away.
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
