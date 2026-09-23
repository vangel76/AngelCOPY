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

// (On-disk policy outcomes live in tests\test_native.cpp's policy matrix —
// the robocopy execution path this file once exercised was removed.)

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

        // THE policy predicate every aggregate and the engine derive from.
        printf("PolicyCopies:\n");
        check(PolicyCopies(Conflict::Replace, FileClass::Lonely) &&
                  PolicyCopies(Conflict::Skip, FileClass::Lonely),
              "Lonely copied under every policy");
        check(!PolicyCopies(Conflict::Replace, FileClass::Same) &&
                  !PolicyCopies(Conflict::Skip, FileClass::Same),
              "Same never copied");
        check(PolicyCopies(Conflict::Replace, FileClass::DiffNewer) &&
                  PolicyCopies(Conflict::ReplaceIfNewer, FileClass::DiffNewer) &&
                  !PolicyCopies(Conflict::Skip, FileClass::DiffNewer),
              "DiffNewer copied except under Skip");
        check(PolicyCopies(Conflict::Replace, FileClass::DiffOlder) &&
                  !PolicyCopies(Conflict::ReplaceIfNewer, FileClass::DiffOlder) &&
                  !PolicyCopies(Conflict::Skip, FileClass::DiffOlder),
              "DiffOlder copied only under Replace");

        // Needed disk space: lonely full, overwrites only their growth. The
        // fixture's files are tiny, so assert the relationships, not literals:
        // Replace >= OnlyIfNewer >= Skip, and Skip == lonely bytes exactly
        // (nothing else is written).
        unsigned long long nRep = NeededSpaceFor(s, Conflict::Replace);
        unsigned long long nNew = NeededSpaceFor(s, Conflict::ReplaceIfNewer);
        unsigned long long nSkp = NeededSpaceFor(s, Conflict::Skip);
        check(nSkp == s.lonelyBytes, "NeededSpace(Skip) = lonely bytes only");
        check(nNew >= nSkp && nRep >= nNew, "NeededSpace: Replace >= Newer >= Skip");
        check(nRep <= s.lonelyBytes + s.newerBytes + s.olderBytes,
              "NeededSpace never exceeds full copied bytes (growth <= size)");
        RmTree(base);
    }

    // ---- overwrite growth: only the size increase counts ----
    {
        std::wstring base = g_root + L"\\grow";
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW((base + L"\\src").c_str(), nullptr);
        CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
        CreateDirectoryW((base + L"\\dest\\src").c_str(), nullptr);
        // src bigger.txt = 1000 B over a 200 B dest -> grows 800.
        // src smaller.txt = 100 B over a 900 B dest -> grows 0 (shrinks).
        // NB: 'small' is a macro in <windows.h> (rpcndr.h: #define small char).
        std::string srcBig(1000, 'B'), srcSmall(100, 's');
        WriteFileText(base + L"\\src\\bigger.txt", srcBig.c_str(), 0);
        WriteFileText(base + L"\\dest\\src\\bigger.txt", std::string(200, 'x').c_str(), 0);
        WriteFileText(base + L"\\src\\smaller.txt", srcSmall.c_str(), 0);
        WriteFileText(base + L"\\dest\\src\\smaller.txt", std::string(900, 'y').c_str(), 0);
        // Make both differ in time so they classify as overwrites, not "same".
        std::vector<std::wstring> sources{base + L"\\src"};
        auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
        ScanResult s = ScanJobs(jobs);
        printf("Overwrite growth:\n");
        // Both are overwrites (different size => not "same"), so 2 conflicts,
        // and the growth is 800 + 0 = 800 regardless of newer/older split.
        check(s.conflicts == 2, "2 overwrites");
        check(NeededSpaceFor(s, Conflict::Replace) == 800,
              "growth = 800 (bigger +800, smaller +0), not 1100 full size");
        RmTree(base);
    }

    RmTree(g_root);
    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
