#pragma once
#include "Robocopy.h"
#include <functional>
#include <vector>

namespace angelcopy {

// Run the jobs while showing a native progress dialog (percentage bar, speed,
// ETA, current file, Cancel). Blocks until the transfer finishes or is
// cancelled. Returns the worst robocopy exit code (>=8 == error); returns a
// negative value if the user cancelled.
// `expectedBytes`/`expectedFiles` must be what robocopy will actually copy
// under `policy` (see ExpectedFor). The bar itself spans expected PLUS skipped
// volume: skipped stretches are painted green and advance the bar as robocopy
// lists them (/V), but never count toward the speed or the ETA.
// `skipSet` (see SkipSetFor) tells a skipped file's output line from a copied
// one; it must outlive the call.
// `skipped` is reported at the end; if anything was skipped (or any error
// occurred) the dialog stays open instead of auto-closing.
// Run `work` (the pre-transfer scans) on a background thread behind a small
// "Preparing — analyzing folders" window with a live file counter and Cancel.
// The window only becomes visible if the work takes longer than ~300 ms, so
// small transfers never flash it. Returns false if the user cancelled
// (prog.cancel is set so the scan aborts; its partial results must be
// discarded). Exists because a 500k-file scan used to run before ANY window,
// leaving the user staring at nothing for seconds.
bool RunScanWithUI(ScanProgress& prog, const std::function<void()>& work);

int RunJobsWithUI(Operation op, const std::wstring& destLabel,
                  const std::vector<RoboJob>& jobs,
                  unsigned long long expectedBytes,
                  unsigned long long expectedFiles, Conflict policy,
                  SkipInfo skipped,
                  const std::unordered_set<std::wstring>& skipSet);

// Permanent delete with the same progress dialog. Caller must have confirmed:
// this bypasses the Recycle Bin and cannot be undone.
int RunDeleteWithUI(const std::vector<std::wstring>& targets,
                    unsigned long long expectedBytes,
                    unsigned long long expectedFiles);

// Mirror: run the copy jobs (Replace policy — a mirror overwrites whatever
// differs), then permanently delete `extraTargets` at the destination, all in
// one dialog. Caller must have confirmed via AskSyncConfirm: the delete phase
// bypasses the Recycle Bin and cannot be undone.
int RunSyncWithUI(const std::vector<RoboJob>& jobs,
                  unsigned long long expectedBytes,
                  unsigned long long expectedFiles, SkipInfo skipped,
                  const std::unordered_set<std::wstring>& skipSet,
                  const std::vector<std::wstring>& extraTargets);

} // namespace angelcopy
