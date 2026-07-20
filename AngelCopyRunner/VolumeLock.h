#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace angelcopy {

// Serializes transfers that touch the same volume. Two AngelCOPY copies onto
// the same slow USB disk don't go twice as fast — they fight over the same
// write ceiling and queue, and each shows a wrong ETA. So a job waits for any
// running job that shares a source or destination volume with it.
//
// Mechanism: one named mutex per drive letter (`Local\AngelCopyVol_X`), held
// for the whole transfer. A mutex — not a lock file — because Windows releases
// it automatically if the owning process dies (WAIT_ABANDONED), so a crash or a
// killed runner never wedges the queue.
//
// Deadlock-free by construction: every job acquires its volumes in sorted
// order, so there is a single global lock order and no cycle can form.
class VolumeLock {
public:
    VolumeLock() = default;
    ~VolumeLock() { Release(); }
    VolumeLock(const VolumeLock&) = delete;
    VolumeLock& operator=(const VolumeLock&) = delete;

    // Acquire every volume in `letters` (already collected + sorted). Blocks
    // until all are free, polling so `cancelled` / `forced` can break the wait:
    //   cancelled() true -> abandon, return false (nothing acquired stays held).
    //   forced()    true -> stop waiting and run WITHOUT the locks (the user
    //                       chose to run in parallel this once); returns true,
    //                       holding only whatever was already free.
    // onWaiting(true) is called the first time a volume is busy so the UI can
    // show the waiting state. Returns true if the transfer may proceed.
    bool Acquire(const std::vector<wchar_t>& letters,
                 const std::function<bool()>& cancelled,
                 const std::function<bool()>& forced,
                 const std::function<void()>& onWaiting);

    void Release();

private:
    std::vector<HANDLE> held_;
};

// Distinct, sorted, upper-cased drive letters touched by these paths (source
// and destination). UNC / driveless paths contribute nothing — network
// contention is a different animal and out of scope here.
std::vector<wchar_t> VolumesForPaths(const std::vector<std::wstring>& paths);

} // namespace angelcopy
