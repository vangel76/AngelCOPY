#include "VolumeLock.h"

#include <algorithm>
#include <cwctype>

namespace angelcopy {

std::vector<wchar_t> VolumesForPaths(const std::vector<std::wstring>& paths) {
    std::vector<wchar_t> out;
    for (const auto& p : paths) {
        if (p.size() >= 2 && p[1] == L':') {
            wchar_t c = (wchar_t)std::towupper(p[0]);
            if (c >= L'A' && c <= L'Z' &&
                std::find(out.begin(), out.end(), c) == out.end())
                out.push_back(c);
        }
        // UNC (\\server\share) and driveless paths: no volume letter, skipped.
    }
    std::sort(out.begin(), out.end()); // global lock order -> no deadlock
    return out;
}

bool VolumeLock::Acquire(const std::vector<wchar_t>& letters,
                         const std::function<bool()>& cancelled,
                         const std::function<bool()>& forced,
                         const std::function<void()>& onWaiting) {
    bool announced = false;
    for (wchar_t c : letters) {
        std::wstring name = L"Local\\AngelCopyVol_";
        name.push_back(c);
        HANDLE m = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!m) continue; // can't create the mutex: don't block the transfer

        for (;;) {
            if (cancelled()) { CloseHandle(m); Release(); return false; }
            if (forced()) {
                // User chose to run in parallel this once: stop acquiring and
                // proceed. Drop this mutex (we won't hold what we didn't get)
                // but keep the ones already acquired so they release cleanly.
                CloseHandle(m);
                return true;
            }
            DWORD r = WaitForSingleObject(m, 200);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
                // WAIT_ABANDONED: previous owner died mid-transfer; the volume
                // is ours now (its half-written file is a separate concern,
                // handled by the next run's classify/overwrite).
                held_.push_back(m);
                break;
            }
            if (r != WAIT_TIMEOUT) { CloseHandle(m); break; } // odd error: skip
            if (!announced) { announced = true; onWaiting(); }
        }
    }
    return true;
}

void VolumeLock::Release() {
    for (HANDLE m : held_) {
        ReleaseMutex(m);
        CloseHandle(m);
    }
    held_.clear();
}

} // namespace angelcopy
