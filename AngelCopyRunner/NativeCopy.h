#pragma once
#include "Robocopy.h"
#include <functional>

namespace angelcopy {

// Native copy engine — replaces robocopy *execution* while keeping the
// planning/scanning layer (PlanJobs, ScanJobs, policies) unchanged. Semantics
// mirror the flags BuildRobocopyArgs used: /E (empty dirs too), /COPY:DAT,
// /R:2 /W:2, /XJ (reparse points skipped), policy classes per ClassifyFile.
//
// Why it exists (measured, bench/bench.cpp, July 2026, NVMe):
//   small files: CopyFileW pool, one thread per directory (256-file chunks so
//     a flat dir still parallelizes), 16 threads: +33% vs robocopy /MT:64.
//     More threads collapse on NTFS same-directory create contention.
//   one big file: unbuffered overlapped ring, QD8 x 8 MiB: +32%.
//   same-volume move: MoveFileExW rename, ~1500x — robocopy always copies.
// CopyFileEx stays the per-file primitive on purpose: Win11 kernel fast-path
// (NtCopyFileChunk) and SMB server-side copy; a manual read/write loop loses
// both. Progress comes from real callbacks — no child process, no pipe
// parsing, no IO-counter detour, and skips are reported exactly.

struct CopySink {
    // Live byte progress of actual copying (never fires for skips/renames of
    // whole trees are reported via onFileDone+onBytes after the fact).
    std::function<void(unsigned long long delta)> onBytes;
    std::function<void(const std::wstring& path)> onFileStart;
    std::function<void(const std::wstring& path, unsigned long long size)> onFileDone;
    // Listed but not copied (identical / policy-excluded) — the green bar.
    std::function<void(const std::wstring& path, unsigned long long size)> onSkip;
    std::function<void(const std::wstring& message)> onError;
    std::function<bool()> cancelled;
};

// Run every job. Returns robocopy-convention codes: 0 = nothing to do,
// 1 = copied/moved something, 8 = at least one file failed.
int RunNativeJobs(Operation op, const std::vector<RoboJob>& jobs,
                  Conflict policy, const CopySink& sink);

// Console-mode dispatcher: native engine by default, robocopy when
// ANGELCOPY_ENGINE=robocopy. Prints per-job lines + errors like RunJobs.
int RunJobsConsole(Operation op, const std::vector<RoboJob>& jobs,
                   Conflict policy);

// False only when ANGELCOPY_ENGINE=robocopy is set (A/B fallback during
// burn-in; the robocopy path stays intact behind it).
bool UseNativeEngine();

// GUI fast path, tried BEFORE any scan or window: a whole-tree move whose
// destination does not exist cannot conflict with anything, and on the same
// volume it is a single rename. Returns true when the job completed that way
// (the caller drops it); cross-volume or existing destinations return false
// and go through the normal scan → prompt → transfer flow.
bool TryQuickRenameMove(const RoboJob& job);

} // namespace angelcopy
