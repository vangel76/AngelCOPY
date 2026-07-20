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

    RmTree(g_root);
    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
