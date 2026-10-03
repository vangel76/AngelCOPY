// Verifies the native engine (NativeCopy.cpp) end-to-end against the real
// filesystem: policy outcomes must match what the robocopy path produced
// (test_conflict.cpp), plus the engine-only behaviors — rename moves, the
// big-file ring, junction safety, skip/byte accounting, cancellation.
//
// Build (same pattern as the other tests):
//   cl /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE test_native.cpp ^
//      ..\AngelCopyRunner\NativeCopy.cpp ..\AngelCopyRunner\Robocopy.cpp ^
//      Shlwapi.lib
#include "../AngelCopyRunner/NativeCopy.h"
#include "../shared/Util.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

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

static bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static unsigned long long SizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return ~0ull;
    return ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static void RmTree(const std::wstring& dir) {
    std::wstring cmd = L"cmd /c rd /s /q \"" + dir + L"\" 2>nul";
    _wsystem(cmd.c_str());
}

// Local \\?\ helper for the long-path test (the engine's own is internal).
static std::wstring Ext(const std::wstring& p) {
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) return p;
    return L"\\\\?\\" + p;
}

// Counting sink shared by the tests; errors print so failures are diagnosable.
struct Counts {
    std::atomic<unsigned long long> bytes{0}, files{0}, skips{0}, skipBytes{0},
        errors{0};
    bool cancel = false;
    CopySink Sink() {
        CopySink s;
        s.onBytes = [this](unsigned long long d) { bytes += d; };
        s.onFileDone = [this](const std::wstring&, unsigned long long) { files += 1; };
        s.onSkip = [this](const std::wstring&, unsigned long long sz) {
            skips += 1;
            skipBytes += sz;
        };
        s.onError = [this](const std::wstring& m) {
            errors += 1;
            printf("    engine error: %ws\n", m.c_str());
        };
        s.cancelled = [this] { return cancel; };
        return s;
    }
};

// ---- policy matrix (mirror of test_conflict.cpp, native engine) ------------

static void SetupConflict(const std::wstring& base) {
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest\\src").c_str(), nullptr);

    WriteFileText(base + L"\\src\\lonely.txt", "FROM-SOURCE", 0);

    WriteFileText(base + L"\\src\\same.txt", "IDENTICAL", 0);
    WriteFileText(base + L"\\dest\\src\\same.txt", "IDENTICAL", 0);
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

    WriteFileText(base + L"\\dest\\src\\newer.txt", "DEST-OLD-DATA", -10);
    WriteFileText(base + L"\\src\\newer.txt", "SRC-NEW", 0);

    WriteFileText(base + L"\\src\\older.txt", "SRC-OLD", -10);
    WriteFileText(base + L"\\dest\\src\\older.txt", "DEST-NEWER-DATA", 0);
}

static void RunPolicy(Conflict policy, const char* name, const char* expNewer,
                      const char* expOlder, unsigned long long expSkips) {
    std::wstring base = g_root + L"\\p_" + std::to_wstring((int)policy);
    SetupConflict(base);

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    RunNativeJobs(Operation::Copy, jobs, policy, c.Sink());

    printf("%s:\n", name);
    std::string newer = ReadText(base + L"\\dest\\src\\newer.txt");
    std::string older = ReadText(base + L"\\dest\\src\\older.txt");
    check(newer == expNewer, (std::string("  newer.txt == ") + expNewer +
                              " (got " + newer + ")").c_str());
    check(older == expOlder, (std::string("  older.txt == ") + expOlder +
                              " (got " + older + ")").c_str());
    check(ReadText(base + L"\\dest\\src\\lonely.txt") == "FROM-SOURCE",
          "  lonely.txt always copied");
    check(c.skips == expSkips, "  skip count matches policy");
    check(c.errors == 0, "  no errors");
    RmTree(base);
}

// ---- tree copy: structure, empty dirs, accounting, re-copy rc --------------

static void TestTreeCopy() {
    printf("tree copy:\n");
    std::wstring base = g_root + L"\\tree";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\src\\a").c_str(), nullptr);
    CreateDirectoryW((base + L"\\src\\a\\deep").c_str(), nullptr);
    CreateDirectoryW((base + L"\\src\\emptydir").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    WriteFileText(base + L"\\src\\top.txt", "TOP", 0);
    WriteFileText(base + L"\\src\\a\\mid.txt", "MID", 0);
    WriteFileText(base + L"\\src\\a\\deep\\leaf.txt", "LEAF", 0);

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    int rc = RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());

    check(rc == 1, "  rc == 1 (copied something)");
    check(ReadText(base + L"\\dest\\src\\a\\deep\\leaf.txt") == "LEAF",
          "  nested file arrived");
    check(IsDir(base + L"\\dest\\src\\emptydir"), "  empty dir created (/E parity)");
    check(c.files == 3 && c.bytes == 3 + 3 + 4, "  bytes/files accounted exactly");

    // Timestamp preserved (/COPY:T parity, 2s tolerance).
    WIN32_FILE_ATTRIBUTE_DATA s{}, d{};
    GetFileAttributesExW((base + L"\\src\\top.txt").c_str(), GetFileExInfoStandard, &s);
    GetFileAttributesExW((base + L"\\dest\\src\\top.txt").c_str(), GetFileExInfoStandard, &d);
    unsigned long long ts = ((unsigned long long)s.ftLastWriteTime.dwHighDateTime << 32) | s.ftLastWriteTime.dwLowDateTime;
    unsigned long long td = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    check((ts > td ? ts - td : td - ts) <= 20000000ULL, "  mtime preserved");

    // Second run: everything identical -> all skips, nothing copied, rc 0.
    Counts c2;
    int rc2 = RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c2.Sink());
    check(rc2 == 0, "  re-copy rc == 0 (nothing to do)");
    check(c2.files == 0 && c2.skips == 3, "  re-copy skips everything");
    RmTree(base);
}

// ---- move: whole-tree rename, policy-skip leftovers, loose file ------------

static void TestMove() {
    printf("move:\n");
    std::wstring base = g_root + L"\\mv";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\src\\sub").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    WriteFileText(base + L"\\src\\f1.txt", "ONE", 0);
    WriteFileText(base + L"\\src\\sub\\f2.txt", "TWO", 0);

    // Whole-tree move, destination absent: must collapse to a rename.
    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Move, base + L"\\dest", sources);
    Counts c;
    int rc = RunNativeJobs(Operation::Move, jobs, Conflict::Replace, c.Sink());
    check(rc == 1, "  rc == 1");
    check(ReadText(base + L"\\dest\\src\\sub\\f2.txt") == "TWO", "  tree arrived");
    check(!Exists(base + L"\\src"), "  source tree gone (rename)");
    check(c.bytes == 6 && c.files == 2, "  rename reported bytes/files");

    // Move into EXISTING dest with Skip policy: conflicting file must survive
    // at the source (robocopy /MOVE parity: excluded files are not deleted).
    CreateDirectoryW((base + L"\\src2").c_str(), nullptr);
    WriteFileText(base + L"\\src2\\keep.txt", "SRC-KEEP", 0);
    WriteFileText(base + L"\\src2\\move.txt", "SRC-MOVE", 0);
    CreateDirectoryW((base + L"\\dest\\src2").c_str(), nullptr);
    WriteFileText(base + L"\\dest\\src2\\keep.txt", "DEST-KEEP", -10);
    std::vector<std::wstring> s2{base + L"\\src2"};
    auto jobs2 = PlanJobs(Operation::Move, base + L"\\dest", s2);
    Counts c2;
    RunNativeJobs(Operation::Move, jobs2, Conflict::Skip, c2.Sink());
    check(ReadText(base + L"\\dest\\src2\\keep.txt") == "DEST-KEEP",
          "  skip policy left destination alone");
    check(ReadText(base + L"\\src2\\keep.txt") == "SRC-KEEP",
          "  skipped file still at source");
    check(ReadText(base + L"\\dest\\src2\\move.txt") == "SRC-MOVE",
          "  lonely file moved");
    check(!Exists(base + L"\\src2\\move.txt"), "  moved file left the source");
    check(Exists(base + L"\\src2"), "  source dir kept (still holds a skip)");

    // GUI fast path: whole-tree rename before any scan (absent destination).
    CreateDirectoryW((base + L"\\src3").c_str(), nullptr);
    WriteFileText(base + L"\\src3\\q.txt", "QUICK", 0);
    RoboJob qr;
    qr.srcDir = base + L"\\src3";
    qr.dstDir = base + L"\\destq\\src3"; // parent doesn't exist either
    check(TryQuickRenameMove(qr), "  quick rename succeeds");
    check(ReadText(base + L"\\destq\\src3\\q.txt") == "QUICK", "  quick rename moved tree");
    check(!Exists(base + L"\\src3"), "  quick rename source gone");
    RoboJob qr2;
    qr2.srcDir = base + L"\\destq\\src3";
    qr2.dstDir = base + L"\\destq\\src3"; // destination exists -> must refuse
    check(!TryQuickRenameMove(qr2), "  quick rename refuses existing dest");

    // Loose-file move (file source -> per-file rename).
    WriteFileText(base + L"\\loose.txt", "LOOSE", 0);
    std::vector<std::wstring> s3{base + L"\\loose.txt"};
    auto jobs3 = PlanJobs(Operation::Move, base + L"\\dest", s3);
    Counts c3;
    RunNativeJobs(Operation::Move, jobs3, Conflict::Replace, c3.Sink());
    check(ReadText(base + L"\\dest\\loose.txt") == "LOOSE", "  loose file moved");
    check(!Exists(base + L"\\loose.txt"), "  loose source gone");
    RmTree(base);
}

// ---- junction safety (/XJ parity) ------------------------------------------

static void TestJunction() {
    printf("junction safety:\n");
    std::wstring base = g_root + L"\\junc";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\precious").c_str(), nullptr);
    WriteFileText(base + L"\\precious\\data.txt", "PRECIOUS", 0);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    WriteFileText(base + L"\\src\\normal.txt", "NORMAL", 0);
    std::wstring mk = L"cmd /c mklink /J \"" + base + L"\\src\\link\" \"" + base +
                      L"\\precious\" >nul";
    _wsystem(mk.c_str());
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);

    if (!Exists(base + L"\\src\\link")) {
        check(false, "  mklink /J failed (cannot test)");
        return;
    }

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    check(ReadText(base + L"\\dest\\src\\normal.txt") == "NORMAL", "  file copied");
    check(!Exists(base + L"\\dest\\src\\link"), "  junction NOT copied/followed");
    check(ReadText(base + L"\\precious\\data.txt") == "PRECIOUS", "  target intact");
    RmTree(base);
}

// ---- big file: overlapped ring ---------------------------------------------

static void TestBigFile() {
    printf("big file (ring):\n");
    std::wstring base = g_root + L"\\big";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);

    // 40 MiB + 1234 bytes: over the 32 MiB ring threshold AND deliberately not
    // sector-aligned, so the trim path is exercised.
    const unsigned long long kSize = (40ull << 20) + 1234;
    std::wstring srcFile = base + L"\\src\\huge.bin";
    {
        HANDLE h = CreateFileW(srcFile.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::vector<unsigned char> chunk(1 << 20);
        unsigned long long written = 0;
        unsigned counter = 0x1234567u;
        while (written < kSize) {
            for (size_t i = 0; i < chunk.size(); i += 4) {
                counter = counter * 1664525u + 1013904223u;
                memcpy(&chunk[i], &counter, 4);
            }
            DWORD n = (DWORD)((kSize - written < chunk.size()) ? kSize - written
                                                               : chunk.size());
            DWORD w = 0;
            WriteFile(h, chunk.data(), n, &w, nullptr);
            written += w;
        }
        CloseHandle(h);
    }

    std::vector<std::wstring> sources{srcFile};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    int rc = RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    std::wstring dstFile = base + L"\\dest\\huge.bin";

    check(rc == 1, "  rc == 1");
    check(SizeOf(dstFile) == kSize, "  exact size (tail trimmed)");
    check(c.bytes == kSize, "  ring reported every byte");

    // Full content compare.
    bool same = true;
    {
        HANDLE a = CreateFileW(srcFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        HANDLE b = CreateFileW(dstFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        std::vector<unsigned char> ba(1 << 20), bb(1 << 20);
        for (;;) {
            DWORD ra = 0, rb = 0;
            BOOL oa = ReadFile(a, ba.data(), (DWORD)ba.size(), &ra, nullptr);
            BOOL ob = ReadFile(b, bb.data(), (DWORD)bb.size(), &rb, nullptr);
            if (!oa || !ob || ra != rb) { same = (oa && ob && ra == rb); break; }
            if (ra == 0) break;
            if (memcmp(ba.data(), bb.data(), ra) != 0) { same = false; break; }
        }
        CloseHandle(a);
        CloseHandle(b);
    }
    check(same, "  content identical");

    // mtime preserved.
    WIN32_FILE_ATTRIBUTE_DATA s{}, d{};
    GetFileAttributesExW(srcFile.c_str(), GetFileExInfoStandard, &s);
    GetFileAttributesExW(dstFile.c_str(), GetFileExInfoStandard, &d);
    unsigned long long ts = ((unsigned long long)s.ftLastWriteTime.dwHighDateTime << 32) | s.ftLastWriteTime.dwLowDateTime;
    unsigned long long td = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    check((ts > td ? ts - td : td - ts) <= 20000000ULL, "  mtime preserved");
    RmTree(base);
}

// ---- read-only destination overwrite ---------------------------------------

static void TestReadonlyDest() {
    printf("read-only destination:\n");
    std::wstring base = g_root + L"\\ro";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    WriteFileText(base + L"\\src\\f.txt", "NEW-DATA", 0);
    WriteFileText(base + L"\\dest\\f.txt", "OLD-RO", -10);
    SetFileAttributesW((base + L"\\dest\\f.txt").c_str(), FILE_ATTRIBUTE_READONLY);

    std::vector<std::wstring> sources{base + L"\\src\\f.txt"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    check(ReadText(base + L"\\dest\\f.txt") == "NEW-DATA",
          "  read-only file overwritten");
    check(c.errors == 0, "  no errors");
    SetFileAttributesW((base + L"\\dest\\f.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
    RmTree(base);
}

// ---- cancel -----------------------------------------------------------------

static void TestCancel() {
    printf("cancel:\n");
    std::wstring base = g_root + L"\\cx";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    WriteFileText(base + L"\\src\\f.txt", "DATA", 0);

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    Counts c;
    c.cancel = true; // cancelled before anything starts
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    check(!Exists(base + L"\\dest\\src\\f.txt"), "  nothing copied after cancel");
    check(c.errors == 0, "  cancel is not an error");
    RmTree(base);
}

// ---- same-folder copy -> "<name> - Copy" -----------------------------------

static void TestSameFolderCopy() {
    printf("same-folder copy:\n");
    std::wstring base = g_root + L"\\sfc";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    WriteFileText(base + L"\\doc.txt", "ORIGINAL", 0);
    CreateDirectoryW((base + L"\\folder").c_str(), nullptr);
    WriteFileText(base + L"\\folder\\inner.txt", "INNER", 0);

    // Loose file pasted into its own folder.
    std::vector<std::wstring> s1{base + L"\\doc.txt"};
    auto j1 = PlanJobs(Operation::Copy, base, s1, L"Copy");
    Counts c1;
    RunNativeJobs(Operation::Copy, j1, Conflict::Replace, c1.Sink());
    check(ReadText(base + L"\\doc.txt") == "ORIGINAL", "  original untouched");
    check(ReadText(base + L"\\doc - Copy.txt") == "ORIGINAL",
          "  made 'doc - Copy.txt'");

    // Paste again -> " (2)".
    auto j2 = PlanJobs(Operation::Copy, base, s1, L"Copy");
    RunNativeJobs(Operation::Copy, j2, Conflict::Replace, Counts{}.Sink());
    check(Exists(base + L"\\doc - Copy (2).txt"), "  second paste -> ' (2)'");

    // Folder pasted into its own parent.
    std::vector<std::wstring> s3{base + L"\\folder"};
    auto j3 = PlanJobs(Operation::Copy, base, s3, L"Copy");
    RunNativeJobs(Operation::Copy, j3, Conflict::Replace, Counts{}.Sink());
    check(ReadText(base + L"\\folder - Copy\\inner.txt") == "INNER",
          "  made 'folder - Copy' with contents");
    check(IsDir(base + L"\\folder"), "  original folder intact");

    // Same-folder MOVE is a no-op (no self-destruction, no rename).
    std::vector<std::wstring> s4{base + L"\\doc.txt"};
    auto j4 = PlanJobs(Operation::Move, base, s4, L"Copy");
    check(j4.empty(), "  same-folder move planned as no-op");
    RunNativeJobs(Operation::Move, j4, Conflict::Replace, Counts{}.Sink());
    check(ReadText(base + L"\\doc.txt") == "ORIGINAL", "  move no-op kept original");
    RmTree(base);
}

// Regression: paths past MAX_PATH must be scanned AND copied. The walkers once
// enumerated/stat'd without the \\?\ prefix while the I/O used it, so long
// paths were silently dropped — a copy reported complete but short, and in a
// mirror an existing source read as "missing" and its destination twin was
// purged. Covers both the whole-tree walk and the loose-file plan/engine pair.
static void TestLongPaths() {
    printf("Long paths (>MAX_PATH):\n");
    std::wstring base = g_root + L"\\longp";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);

    // Build a source tree whose leaf path exceeds MAX_PATH. Each level needs
    // the \\?\ form to be created at all.
    std::wstring seg(60, L'x');
    std::wstring deep = base + L"\\src";
    CreateDirectoryW(Ext(deep).c_str(), nullptr);
    for (int i = 0; i < 5; ++i) {          // 5 * 61 chars past the base
        deep += L"\\" + seg;
        CreateDirectoryW(Ext(deep).c_str(), nullptr);
    }
    check(deep.size() > MAX_PATH, "  (setup) source path exceeds MAX_PATH");
    WriteFileText(Ext(deep + L"\\deep.txt"), "DEEPDATA", 0);
    check(Exists(Ext(deep + L"\\deep.txt")), "  (setup) deep file created");

    // Whole-tree copy: the scan must count it and the engine must copy it.
    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dst", sources, L"Copy");
    check(!jobs.empty() && jobs[0].files.empty(),
          "  deep source planned as a whole-tree job (IsDirectory saw it)");

    ScanResult scan = ScanJobs(jobs, nullptr);
    check(scan.lonelyFiles == 1, "  scan counted the deep file");

    Counts c;
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    // PlanJobs maps <src> onto <destDir>\<basename(src)>, so the tree lands
    // under dst\src, not directly under dst.
    std::wstring dstDeep = base + L"\\dst\\src";
    for (int i = 0; i < 5; ++i) dstDeep += L"\\" + seg;
    check(ReadText(Ext(dstDeep + L"\\deep.txt")) == "DEEPDATA",
          "  deep file actually copied");
    check(c.files.load() == 1, "  engine reported the deep file (no silent drop)");
    check(c.errors.load() == 0, "  no errors");

    // Second case: the SOURCE ROOT ITSELF is past MAX_PATH (user selects the
    // deep folder directly). This is what exercises PlanJobs' IsDirectory —
    // with a bare GetFileAttributesW it reads as "not a directory", the folder
    // is planned as a LOOSE FILE, and nothing is copied. The first case above
    // cannot catch that: its source root is short.
    {
        std::vector<std::wstring> deepSrc{deep};              // > MAX_PATH
        auto j = PlanJobs(Operation::Copy, base + L"\\dst2", deepSrc, L"Copy");
        check(j.size() == 1 && j[0].files.empty(),
              "  long source ROOT planned as a whole-tree job, not a loose file");

        Counts c2;
        RunNativeJobs(Operation::Copy, j, Conflict::Replace, c2.Sink());
        check(Exists(Ext(base + L"\\dst2\\" + seg + L"\\deep.txt")),
              "  long source ROOT actually copied");
        check(c2.errors.load() == 0, "  no errors (long source root)");
    }

    RmTree(base);
}

// The native copy engine must skip excluded folders (Unreal preset), matching
// the scan. Verifies WalkStream honors IsExcludedDir end-to-end.
static void TestExcludeDirs() {
    printf("Excluded folders (Unreal preset):\n");
    std::wstring base = g_root + L"\\excl";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\src\\Content").c_str(), nullptr);
    WriteFileText(base + L"\\src\\Content\\a.uasset", "asset", 0);
    CreateDirectoryW((base + L"\\src\\Intermediate").c_str(), nullptr);
    WriteFileText(base + L"\\src\\Intermediate\\junk.tmp", "junk", 0);
    CreateDirectoryW((base + L"\\src\\Intermediate\\deep").c_str(), nullptr);
    WriteFileText(base + L"\\src\\Intermediate\\deep\\more.tmp", "more", 0);

    SetExcludedDirs(UnrealExcludeNames());
    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dst", sources, L"Copy");
    Counts c;
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    SetExcludedDirs({}); // reset global

    check(Exists(base + L"\\dst\\src\\Content\\a.uasset"),
          "  Content copied");
    check(!Exists(base + L"\\dst\\src\\Intermediate"),
          "  Intermediate folder NOT copied (whole subtree skipped)");
    check(c.files.load() == 1, "  exactly one file copied (only the asset)");
    check(c.errors.load() == 0, "  no errors");
    RmTree(base);
}

// Carried scan verdicts (SetCarriedClasses): the engine must CONSUME them —
// proven adversarially with a deliberately WRONG "Same" verdict for a changed
// file: consuming the carry skips it, re-statting would copy it. A lookup
// miss must fall back to a real classification, and an over-cap map must be
// discarded.
static void TestCarriedClasses() {
    printf("carried scan verdicts:\n");
    std::wstring base = g_root + L"\\carry";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest\\src").c_str(), nullptr);

    // Both files differ at the destination — Replace would copy both.
    WriteFileText(base + L"\\dest\\src\\changed.txt", "DEST-OLD", -10);
    WriteFileText(base + L"\\src\\changed.txt", "SRC-NEW", 0);
    WriteFileText(base + L"\\dest\\src\\missfile.txt", "DEST-OLD", -10);
    WriteFileText(base + L"\\src\\missfile.txt", "SRC-NEW", 0);

    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);

    std::unordered_map<std::wstring, FileClass> m;
    m[acutil::LowerCopy(base + L"\\src\\changed.txt")] = FileClass::Same;
    SetCarriedClasses(std::move(m));

    Counts c;
    RunNativeJobs(Operation::Copy, jobs, Conflict::Replace, c.Sink());
    check(ReadText(base + L"\\dest\\src\\changed.txt") == "DEST-OLD",
          "  carried verdict consumed (engine skipped per the carry)");
    check(ReadText(base + L"\\dest\\src\\missfile.txt") == "SRC-NEW",
          "  lookup miss fell back to a real classification (copied)");
    check(c.skips.load() == 1, "  exactly the carried file counted as a skip");
    check(c.errors.load() == 0, "  no errors");

    // A map over the memory cap must be discarded entirely.
    std::unordered_map<std::wstring, FileClass> big;
    big.reserve(kMaxCarriedClasses + 2);
    for (size_t i = 0; i <= kMaxCarriedClasses; ++i)
        big.emplace(L"k" + std::to_wstring(i), FileClass::Same);
    SetCarriedClasses(std::move(big));
    check(!AnyCarriedClasses(), "  over-cap map discarded (memory cap)");

    SetCarriedClasses({}); // reset for anything that runs after this test
    RmTree(base);
}

// ---- per-file conflict decisions (image compare dialog) ---------------------

static void WriteFilled(const std::wstring& path, unsigned long long size,
                        unsigned char fill, int dayOffset) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<unsigned char> chunk(1 << 20, fill);
    for (unsigned long long done = 0; done < size;) {
        DWORD n = (DWORD)std::min<unsigned long long>(chunk.size(), size - done);
        DWORD w = 0;
        WriteFile(h, chunk.data(), n, &w, nullptr);
        done += n;
    }
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

static unsigned char FirstByte(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned char b = 0;
    DWORD r = 0;
    ReadFile(h, &b, 1, &r, nullptr);
    CloseHandle(h);
    return b;
}

// Overwrite / Skip / Rename decisions must override the policy for exactly
// their file — on the deferred pool path (whole tree), the inline big-file
// path, and the loose-file plan — while undecided conflicts follow the
// policy. Policy Skip makes every decision observable: without the override
// nothing would be written at all.
static void TestFileDecisions() {
    printf("per-file decisions:\n");
    const std::wstring base = g_root + L"\\dec";
    for (int loose = 0; loose < 2; ++loose) {
        RmTree(base);
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW((base + L"\\src").c_str(), nullptr);
        CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
        const std::wstring S = base + L"\\src";
        const std::wstring D = loose ? base + L"\\dest" : base + L"\\dest\\src";
        CreateDirectoryW(D.c_str(), nullptr);
        for (const wchar_t* n : {L"a.jpg", L"b.jpg", L"c.jpg", L"d.txt"}) {
            WriteFileText(D + L"\\" + n, "DEST-OLD", -10);
            WriteFileText(S + L"\\" + n, "SRC-NEW", 0);
        }
        // Over the ring threshold: classified inline on the walk thread.
        const unsigned long long kBig = (33ull << 20) + 7;
        WriteFilled(D + L"\\big.png", 1234, 0xDD, -10);
        WriteFilled(S + L"\\big.png", kBig, 0x5A, 0);

        std::unordered_map<std::wstring, FileDecision> m;
        auto put = [&](const wchar_t* n, FileAction a, const wchar_t* nn) {
            FileDecision d;
            d.action = a;
            d.newName = nn;
            m[acutil::LowerCopy(S + L"\\" + n)] = d;
        };
        put(L"a.jpg", FileAction::Overwrite, L"");
        put(L"b.jpg", FileAction::Skip, L"");
        put(L"c.jpg", FileAction::Rename, L"c (2).jpg");
        put(L"big.png", FileAction::Rename, L"big (2).png");
        SetFileDecisions(std::move(m));

        std::vector<std::wstring> sources;
        if (loose)
            for (const wchar_t* n : {L"a.jpg", L"b.jpg", L"c.jpg", L"d.txt", L"big.png"})
                sources.push_back(S + L"\\" + n);
        else
            sources.push_back(S);
        auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
        Counts c;
        RunNativeJobs(Operation::Copy, jobs, Conflict::Skip, c.Sink());

        printf(loose ? " loose files:\n" : " whole tree:\n");
        check(ReadText(D + L"\\a.jpg") == "SRC-NEW", "  Overwrite beat policy Skip");
        check(ReadText(D + L"\\b.jpg") == "DEST-OLD", "  Skip kept the destination");
        check(ReadText(D + L"\\c.jpg") == "DEST-OLD", "  Rename left the original alone");
        check(ReadText(D + L"\\c (2).jpg") == "SRC-NEW", "  Rename wrote 'c (2).jpg'");
        check(ReadText(D + L"\\d.txt") == "DEST-OLD", "  undecided file followed the policy");
        check(SizeOf(D + L"\\big.png") == 1234, "  big file: original alone");
        check(SizeOf(D + L"\\big (2).png") == kBig &&
                  FirstByte(D + L"\\big (2).png") == 0x5A,
              "  big file: renamed copy via the ring");
        check(c.skips == 2, "  skips = b.jpg + d.txt");
        check(c.errors == 0, "  no errors");
    }

    // Move + Rename: source leaves, original stays, renamed copy arrives.
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW((base + L"\\src").c_str(), nullptr);
    CreateDirectoryW((base + L"\\dest").c_str(), nullptr);
    WriteFileText(base + L"\\dest\\p.jpg", "DEST-OLD", -10);
    WriteFileText(base + L"\\src\\p.jpg", "SRC-NEW", 0);
    // A file already sits under the reserved name: noReplace must refuse it.
    WriteFileText(base + L"\\dest\\q.jpg", "DEST-OLD", -10);
    WriteFileText(base + L"\\dest\\q (2).jpg", "PRECIOUS", -10);
    WriteFileText(base + L"\\src\\q.jpg", "SRC-NEW", 0);
    {
        std::unordered_map<std::wstring, FileDecision> m;
        FileDecision d;
        d.action = FileAction::Rename;
        d.newName = L"p (2).jpg";
        m[acutil::LowerCopy(base + L"\\src\\p.jpg")] = d;
        d.newName = L"q (2).jpg"; // deliberately taken (appeared after the prompt)
        m[acutil::LowerCopy(base + L"\\src\\q.jpg")] = d;
        SetFileDecisions(std::move(m));
    }
    std::vector<std::wstring> mv{base + L"\\src\\p.jpg", base + L"\\src\\q.jpg"};
    auto jm = PlanJobs(Operation::Move, base + L"\\dest", mv);
    Counts cm;
    RunNativeJobs(Operation::Move, jm, Conflict::Replace, cm.Sink());
    printf(" move + rename:\n");
    check(!Exists(base + L"\\src\\p.jpg"), "  source moved away");
    check(ReadText(base + L"\\dest\\p.jpg") == "DEST-OLD", "  original untouched");
    check(ReadText(base + L"\\dest\\p (2).jpg") == "SRC-NEW", "  arrived as 'p (2).jpg'");
    check(ReadText(base + L"\\dest\\q (2).jpg") == "PRECIOUS",
          "  taken rename target NOT overwritten");
    check(ReadText(base + L"\\src\\q.jpg") == "SRC-NEW", "  its source kept");
    check(cm.errors == 1, "  the refusal is reported as an error");

    SetFileDecisions({});
    RmTree(base);
}

// RenamePlanner: "(n)" names avoid existing destination files, files still
// coming in (same source folder, or another loose job into the same folder)
// and names it already handed out.
static void TestRenamePlanner() {
    printf("rename planner:\n");
    const std::wstring base = g_root + L"\\rp";
    RmTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    for (const wchar_t* d : {L"\\src", L"\\other", L"\\dest"})
        CreateDirectoryW((base + d).c_str(), nullptr);
    WriteFileText(base + L"\\dest\\a.jpg", "D", 0);
    WriteFileText(base + L"\\dest\\a (2).jpg", "D", 0);
    WriteFileText(base + L"\\src\\a.jpg", "S", 0);
    WriteFileText(base + L"\\src\\a (3).jpg", "S", 0);     // incoming, same folder
    WriteFileText(base + L"\\dest\\x.jpg", "D", 0);
    WriteFileText(base + L"\\src\\x.jpg", "S", 0);
    WriteFileText(base + L"\\other\\x (2).jpg", "S", 0);   // incoming, other job

    std::vector<std::wstring> sources{base + L"\\src\\a.jpg", base + L"\\src\\x.jpg",
                                      base + L"\\other\\x (2).jpg"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    RenamePlanner rp(jobs);
    ConflictItem a{base + L"\\src\\a.jpg", base + L"\\dest\\a.jpg", 1, 1,
                   FileClass::DiffNewer};
    ConflictItem x{base + L"\\src\\x.jpg", base + L"\\dest\\x.jpg", 1, 1,
                   FileClass::DiffNewer};
    check(rp.Peek(a) == L"a (4).jpg" && rp.Peek(a) == L"a (4).jpg",
          "  Peek shows the name without reserving it");
    check(rp.Reserve(a) == L"a (4).jpg", "  skips existing (2) and incoming (3)");
    check(rp.Reserve(a) == L"a (5).jpg", "  never hands out a name twice");
    check(rp.Reserve(x) == L"x (3).jpg", "  skips a name another job brings in");
    check(NumberedName(L"noext", 2) == L"noext (2)", "  NumberedName without extension");
    check(NumberedName(L"a.tar.gz", 2) == L"a.tar (2).gz", "  NumberedName: last dot");
    check(IsImageFile(L"C:\\x\\IMG.JPG") && IsImageFile(L"r.cr3") &&
              !IsImageFile(L"doc.txt") && !IsImageFile(L"C:\\a.jpg\\noext"),
          "  IsImageFile by extension, case-insensitive");
    RmTree(base);
}

// Totals corrected for decisions: what the progress bar, the skip report and
// the space check see must match what the engine will do.
static void TestAdjustForDecisions() {
    printf("decision totals:\n");
    std::vector<ConflictItem> items = {
        {L"C:\\s\\o.jpg", L"C:\\d\\o.jpg", 100, 40, FileClass::DiffNewer},
        {L"C:\\s\\k.jpg", L"C:\\d\\k.jpg", 200, 10, FileClass::DiffOlder},
        {L"C:\\s\\r.jpg", L"C:\\d\\r.jpg", 300, 290, FileClass::DiffNewer},
        {L"C:\\s\\u.txt", L"C:\\d\\u.txt", 50, 0, FileClass::DiffNewer},
    };
    std::unordered_map<std::wstring, FileDecision> m;
    FileDecision d;
    d.action = FileAction::Skip;      m[L"c:\\s\\o.jpg"] = d;
    d.action = FileAction::Overwrite; m[L"c:\\s\\k.jpg"] = d;
    d.action = FileAction::Rename;    d.newName = L"r (2).jpg"; m[L"c:\\s\\r.jpg"] = d;
    SetFileDecisions(std::move(m));

    // Policy Replace copies all four: bytes 650, files 4, need = growth 60+190+10+50.
    unsigned long long bytes = 650, files = 4, need = 310;
    SkipInfo k;
    AdjustForDecisions(items, Conflict::Replace, bytes, files, k, need);
    check(bytes == 550 && files == 3, "  skip removed o.jpg from the copy totals");
    check(k.policyFiles == 1 && k.policyBytes == 100 && k.byChoice,
          "  ...and moved it into the skip line (by choice)");
    check(need == 310 - 60 + (300 - 10), "  rename costs full size, skip frees its growth");

    // Policy Skip copies none: the Overwrite and Rename picks add back.
    bytes = 0; files = 0; need = 0;
    SkipInfo k2;
    k2.policyFiles = 4; k2.policyBytes = 650;
    AdjustForDecisions(items, Conflict::Skip, bytes, files, k2, need);
    check(bytes == 500 && files == 2, "  overwrite + rename added under policy Skip");
    check(k2.policyFiles == 2 && k2.policyBytes == 150, "  o.jpg + u.txt still skipped");
    check(need == 190 + 300, "  overwrite growth + rename full size");
    SetFileDecisions({});
}

// The scan collects every conflict (with sizes) when asked, so the compare
// dialog can walk them and the totals can be corrected.
static void TestConflictItems() {
    printf("scan conflict items:\n");
    const std::wstring base = g_root + L"\\ci";
    SetupConflict(base);
    std::vector<std::wstring> sources{base + L"\\src"};
    auto jobs = PlanJobs(Operation::Copy, base + L"\\dest", sources);
    SetCollectConflicts(true);
    ScanResult r = ScanJobs(jobs);
    SetCollectConflicts(false);
    check(r.conflicts == 2 && r.conflictItemsComplete(), "  both conflicts collected");
    bool sizes = true;
    for (const auto& c : r.conflictItems)
        sizes = sizes && c.size == SizeOf(c.src) && c.dstSize == SizeOf(c.dst);
    check(sizes, "  source and destination sizes recorded");
    ScanResult off = ScanJobs(jobs);
    check(off.conflictItems.empty() && !off.conflictItemsComplete(),
          "  collection off by default (not offered)");
    RmTree(base);
}

int wmain() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"acp_native";
    RmTree(g_root);
    CreateDirectoryW(g_root.c_str(), nullptr);

    RunPolicy(Conflict::Replace, "policy Replace", "SRC-NEW", "SRC-OLD", 1);
    RunPolicy(Conflict::ReplaceIfNewer, "policy ReplaceIfNewer", "SRC-NEW",
              "DEST-NEWER-DATA", 2);
    RunPolicy(Conflict::Skip, "policy Skip", "DEST-OLD-DATA", "DEST-NEWER-DATA", 3);
    TestTreeCopy();
    TestMove();
    TestJunction();
    TestBigFile();
    TestReadonlyDest();
    TestCancel();
    TestSameFolderCopy();
    TestLongPaths();
    TestExcludeDirs();
    TestCarriedClasses();
    TestFileDecisions();
    TestRenamePlanner();
    TestAdjustForDecisions();
    TestConflictItems();

    RmTree(g_root);
    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
