// VolumeLock unit tests: volume extraction, mutual exclusion on a shared
// volume, sorted acquisition (deadlock-free order), and the cancel / force
// short-circuits.
//
//   cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE ^
//      tests\test_vlock.cpp AngelCopyRunner\VolumeLock.cpp /Fe:build\test_vlock.exe
//   build\test_vlock.exe

#include "../AngelCopyRunner/VolumeLock.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace angelcopy;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static auto never = [] { return false; };
static auto noop = [] {};

int wmain() {
    printf("VolumesForPaths:\n");
    {
        auto v = VolumesForPaths({L"D:\\a", L"d:\\b", L"F:\\x", L"\\\\srv\\share",
                                  L"C:\\y"});
        // D (from D: and d:), F, C -> 3 distinct; the UNC path adds none.
        check(v.size() == 3, "dedup (D+d) + UNC skipped -> 3 volumes");
        std::wstring s(v.begin(), v.end());
        check(s == L"CDF", "sorted, upper-cased, deduped -> \"CDF\"");
        auto unc = VolumesForPaths({L"\\\\server\\share\\x"});
        check(unc.empty(), "UNC path contributes no volume");
        auto empty = VolumesForPaths({});
        check(empty.empty(), "no paths -> no volumes");
    }

    printf("Mutual exclusion on a shared volume:\n");
    {
        // Use a drive letter unlikely to collide with a real transfer during
        // the test window. The mutex name is per-letter, process-global.
        std::vector<wchar_t> vol{L'Q'};
        std::atomic<int> concurrent{0}, maxConcurrent{0}, done{0};

        auto worker = [&] {
            VolumeLock lock;
            bool ok = lock.Acquire(vol, never, never, noop);
            if (!ok) return;
            int c = ++concurrent;
            int prev = maxConcurrent.load();
            while (c > prev && !maxConcurrent.compare_exchange_weak(prev, c)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            --concurrent;
            ++done;
        };

        std::vector<std::thread> ts;
        for (int i = 0; i < 6; ++i) ts.emplace_back(worker);
        for (auto& t : ts) t.join();

        check(done.load() == 6, "all 6 acquired eventually");
        check(maxConcurrent.load() == 1,
              "never more than 1 holder of the same volume at once");
    }

    printf("Force short-circuits the wait:\n");
    {
        std::vector<wchar_t> vol{L'Q'};
        VolumeLock holder;
        holder.Acquire(vol, never, never, noop); // occupy Q

        std::atomic<bool> waited{false};
        bool forced = true;
        VolumeLock second;
        // forced() true -> Acquire returns immediately without the lock.
        bool ok = second.Acquire(vol, never, [&] { return forced; },
                                 [&] { waited = true; });
        check(ok, "forced acquire returns true (proceed in parallel)");
        holder.Release();
    }

    printf("Cancel short-circuits the wait:\n");
    {
        std::vector<wchar_t> vol{L'Q'};
        VolumeLock holder;
        holder.Acquire(vol, never, never, noop);

        bool cancel = true;
        VolumeLock second;
        bool ok = second.Acquire(vol, [&] { return cancel; }, never, noop);
        check(!ok, "cancelled acquire returns false (nothing proceeds)");
        holder.Release();
    }

    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
