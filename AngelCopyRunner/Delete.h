#pragma once
#include "Robocopy.h" // ScanProgress
#include <functional>
#include <string>
#include <vector>

namespace angelcopy {

// Permanent recursive delete. Deliberately NOT robocopy: measured, robocopy's
// /MIR "fast delete" trick is no faster than a plain delete (/MT:64 and /MT:1
// are identical — robocopy does not parallelize its purge phase), and it needs
// a scratch empty directory. Deletion is NTFS-metadata bound, so the win over
// Explorer (~3x) comes purely from skipping the shell's per-item overhead.
//
// This bypasses the Recycle Bin: it cannot be undone. Callers must confirm.

struct DeleteScan {
    unsigned long long files = 0;
    unsigned long long dirs = 0;
    unsigned long long bytes = 0;
    std::vector<std::wstring> sample; // top-level items, for the prompt
};

// Count what would be deleted (recursive, does not follow junctions).
// `prog` (optional) feeds the "Preparing" window and allows cancellation.
DeleteScan ScanDelete(const std::vector<std::wstring>& targets,
                      ScanProgress* prog = nullptr);

struct DeleteSink {
    std::function<void(const std::wstring& path, unsigned long long size)> onFile;
    std::function<void(const std::wstring& message)> onError;
    std::function<bool()> cancelled;
};

// Returns 0 on success, 8 if some items could not be deleted (robocopy-style
// convention, so the progress dialog treats it like a copy failure).
int DeleteTargets(const std::vector<std::wstring>& targets, const DeleteSink& sink);

} // namespace angelcopy
