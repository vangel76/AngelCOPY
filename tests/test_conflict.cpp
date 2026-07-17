// Verifies conflict classification and that each Conflict policy produces the
// real on-disk outcome. Links Robocopy.cpp directly and actually runs robocopy.
#include "../AngelCopyRunner/Robocopy.h"

#include <windows.h>
#include <cstdio>
#include <string>

using namespace angelcopy;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::wstring g_root;

static void WriteFileText(const std::wstring& path, const char* text,
                          int dayOffset) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { printf("cannot write %ws\n", path.c_str()); return; }
    DWORD w = 0;
    WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
    if (dayOffset != 0) {
        FILETIME ft;
        SYSTEMTIME st;
        GetSystemTime(&st);
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart += (long long)dayOffset * 24LL * 3600LL * 10000000LL;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        SetFileTime(h, nullptr, nullptr, &ft);
    }
    CloseHandle(h);
}

static std::string ReadText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    char buf[256]{};
    DWORD r = 0;
    ReadFile(h, buf, 255, &r, nullptr);
    CloseHandle(h);
    return std::string(buf, r);
}

static void RmTree(const std::wstring& dir) {
    std::wstring cmd = L"cmd /c rd /s /q \"" + dir + L"\" 2>nul";
    _wsystem(cmd.c_str());
}

// Build: src has lonely + same + newer + older; dest pre-populated.
static void Setup(const std::wstring& base) {
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest\\src").c_str(), nullptr);

    WriteFileText(base + L"\\src\\lonely.txt", "FROM-SOURCE", 0);

    WriteFileText(base + L"\\src\\same.txt", "IDENTICAL", 0);
    WriteFileText(base + L"\\dest\\src\\same.txt", "IDENTICAL", 0);
    // make timestamps match exactly
    {
        HANDLE a = CreateFileW((base + L"\\src\\same.txt").c_str(), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME ft{};
        GetFileTime(a, nullptr, nullptr, &ft);
        CloseHandle(a);
        HANDLE b = CreateFileW((base + L"\\dest\\src\\same.txt").c_str(),
                               FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        SetFileTime(b, nullptr, nullptr, &ft);
        CloseHandle(b);
    }

    // source NEWER than dest
    WriteFileText(base + L"\\dest\\src\\newer.txt", "DEST-OLD-DATA", -10);
    WriteFileText(base + L"\\src\\newer.txt", "SRC-NEW", 0);

    // source OLDER than dest
    WriteFileText(base + L"\\src\\older.txt", "SRC-OLD", -10);
    WriteFileText(base + L"\\dest\\src\\older.txt", "DEST-NEWER-DATA", 0);
}

static void RunPolicy(Conflict policy, const char* name, const char* expNewer,
                      const char* expOlder) {
    std::wstring base = g_root + L"\\p_" + std::to_wstring((int)policy);
    Setup(base);

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    RunJobs(Operation::Copy, jobs, policy);

    printf("%s:\n", name);
    std::string newer = ReadText(base + L"\\dest\\src\\newer.txt");
    std::string older = ReadText(base + L"\\dest\\src\\older.txt");
    std::string lonely = ReadText(base + L"\\dest\\src\\lonely.txt");
    check(newer == expNewer, (std::string("  newer.txt == ") + expNewer +
                              " (got " + newer + ")").c_str());
    check(older == expOlder, (std::string("  older.txt == ") + expOlder +
                              " (got " + older + ")").c_str());
    check(lonely == "FROM-SOURCE", "  lonely.txt always copied");
    RmTree(base);
}

int wmain() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"acp_conf";
    RmTree(g_root);
    CreateDirectoryW(g_root.c_str(), nullptr);

    // ---- classification ----
    {
        std::wstring base = g_root + L"\\scan";
        Setup(base);
        std::vector<std::wstring> sources{base + L"\\src"};
        auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
        ScanResult s = ScanJobs(jobs);
        printf("Classification:\n");
        check(s.lonelyFiles == 1, "1 lonely file");
        check(s.sameFiles == 1, "1 identical file (not a conflict)");
        check(s.newerFiles == 1, "1 differing file, source newer");
        check(s.olderFiles == 1, "1 differing file, source older");
        check(s.conflicts == 2, "2 conflicts (identical excluded)");

        unsigned long long b = 0, f = 0;
        ExpectedFor(s, Conflict::Replace, b, f);
        check(f == 3, "ExpectedFor(Replace) = lonely+newer+older = 3 files");
        ExpectedFor(s, Conflict::ReplaceIfNewer, b, f);
        check(f == 2, "ExpectedFor(OnlyIfNewer) = lonely+newer = 2 files");
        ExpectedFor(s, Conflict::Skip, b, f);
        check(f == 1, "ExpectedFor(Skip) = lonely only = 1 file");

        // The skip set feeds the progress UI's green (skipped) stretches; it
        // must mirror SkippedFor exactly and hold lowercased SOURCE paths.
        auto low = [](std::wstring w) {
            for (auto& ch : w) ch = (wchar_t)towlower(ch);
            return w;
        };
        auto setRep = SkipSetFor(s, Conflict::Replace);
        auto setNew = SkipSetFor(s, Conflict::ReplaceIfNewer);
        auto setSkp = SkipSetFor(s, Conflict::Skip);
        check(setRep.size() == 1 &&
                  setRep.count(low(base + L"\\src\\same.txt")) == 1,
              "SkipSetFor(Replace) = same.txt only");
        check(setNew.size() == 2 &&
                  setNew.count(low(base + L"\\src\\older.txt")) == 1,
              "SkipSetFor(OnlyIfNewer) = same + older");
        check(setSkp.size() == 3 &&
                  setSkp.count(low(base + L"\\src\\newer.txt")) == 1,
              "SkipSetFor(Skip) = same + newer + older");
        check(setRep.count(low(base + L"\\src\\lonely.txt")) == 0,
              "lonely files never in a skip set");
        RmTree(base);
    }

    // ---- flags ----
    {
        RoboJob j;
        j.srcDir = L"C:\\a"; j.dstDir = L"C:\\b";
        printf("Flags:\n");
        auto rep = BuildRobocopyArgs(Operation::Copy, j, true, Conflict::Replace);
        auto skip = BuildRobocopyArgs(Operation::Copy, j, true, Conflict::Skip);
        auto newer = BuildRobocopyArgs(Operation::Copy, j, true, Conflict::ReplaceIfNewer);
        check(rep.find(L"/XO") == std::wstring::npos, "Replace: no /XO");
        check(skip.find(L"/XC /XN /XO") != std::wstring::npos, "Skip: /XC /XN /XO");
        check(newer.find(L"/XO") != std::wstring::npos &&
              newer.find(L"/XN") == std::wstring::npos, "OnlyIfNewer: /XO, no /XN");
        check(rep.find(L"/MT:64") != std::wstring::npos, "always /MT:64");
    }

    // ---- real on-disk outcomes ----
    RunPolicy(Conflict::Replace, "Replace all", "SRC-NEW", "SRC-OLD");
    RunPolicy(Conflict::ReplaceIfNewer, "Only if newer", "SRC-NEW", "DEST-NEWER-DATA");
    RunPolicy(Conflict::Skip, "Skip existing", "DEST-OLD-DATA", "DEST-NEWER-DATA");

    RmTree(g_root);
    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
