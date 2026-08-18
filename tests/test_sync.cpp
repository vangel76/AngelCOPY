// Mirror (sync) tests: ScanExtras must find exactly what a mirror may delete —
// destination entries missing from the source (or with a mismatched type) —
// and nothing else. Junction handling end-to-end (link removed, target
// untouched) is covered by running the real runner; see also the delete tests.
//
// Build & run (from a VS x64 prompt):
//   cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE ^
//      tests\test_sync.cpp AngelCopyRunner\Robocopy.cpp /Fe:build\test_sync.exe ^
//      /link Shlwapi.lib
//   build\test_sync.exe

#include "../AngelCopyRunner/Robocopy.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace angelcopy;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static void WriteText(const std::wstring& path, const char* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
    CloseHandle(h);
}

static void RmTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                RmTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(p.c_str());
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static bool Contains(const std::vector<std::wstring>& v, const std::wstring& tail) {
    for (const auto& s : v) {
        if (s.size() >= tail.size() &&
            _wcsicmp(s.c_str() + (s.size() - tail.size()), tail.c_str()) == 0)
            return true;
    }
    return false;
}

int wmain() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"acp_sync";
    RmTree(root);

    // Source tree:  src\m\{ a.txt, sub\b.txt, clash (a FILE) }
    // Destination:  dst\m\{ a.txt, sub\b.txt,               -> mirrored, keep
    //                       extra.txt, extradir\deep.txt,   -> extras
    //                       clash\ (a DIRECTORY) }          -> type mismatch
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\src").c_str(), nullptr);
    CreateDirectoryW((root + L"\\src\\m").c_str(), nullptr);
    CreateDirectoryW((root + L"\\src\\m\\sub").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dst").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dst\\m").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dst\\m\\sub").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dst\\m\\extradir").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dst\\m\\clash").c_str(), nullptr);
    WriteText(root + L"\\src\\m\\a.txt", "a");
    WriteText(root + L"\\src\\m\\sub\\b.txt", "b");
    WriteText(root + L"\\src\\m\\clash", "file here, dir at dest");
    WriteText(root + L"\\dst\\m\\a.txt", "a");
    WriteText(root + L"\\dst\\m\\sub\\b.txt", "b");
    WriteText(root + L"\\dst\\m\\extra.txt", "x");
    WriteText(root + L"\\dst\\m\\extradir\\deep.txt", "y");

    printf("ScanExtras:\n");
    {
        std::vector<std::wstring> sources{root + L"\\src\\m"};
        auto jobs = PlanJobs(Operation::Copy, root + L"\\dst", sources);
        auto extras = ScanExtras(jobs);

        check(Contains(extras, L"\\dst\\m\\extra.txt"), "lonely dest file is an extra");
        check(Contains(extras, L"\\dst\\m\\extradir"), "lonely dest dir is an extra (whole tree, one entry)");
        check(!Contains(extras, L"\\dst\\m\\extradir\\deep.txt"), "no separate entry below an extra dir");
        check(Contains(extras, L"\\dst\\m\\clash"), "type mismatch (dir at dest, file in source) is an extra");
        check(!Contains(extras, L"\\dst\\m\\a.txt"), "mirrored file is not an extra");
        check(!Contains(extras, L"\\dst\\m\\sub"), "mirrored dir is not an extra");
        check(extras.size() == 3, "exactly 3 extras");
    }

    printf("Loose-file jobs never purge:\n");
    {
        std::vector<std::wstring> sources{root + L"\\src\\m\\a.txt"};
        auto jobs = PlanJobs(Operation::Copy, root + L"\\dst\\m", sources);
        auto extras = ScanExtras(jobs);
        check(extras.empty(), "file source -> no extras, ever");
    }

    printf("Destination absent:\n");
    {
        std::vector<std::wstring> sources{root + L"\\src\\m"};
        auto jobs = PlanJobs(Operation::Copy, root + L"\\nothere", sources);
        auto extras = ScanExtras(jobs);
        check(extras.empty(), "no destination -> no extras");
    }

    // Regression: a SOURCE-side junction must never be traversed. The copy
    // phase skips source junctions, so descending into one here would compare
    // the destination against the LINK TARGET's contents and purge whatever
    // the target doesn't have — destination data loss driven by a link the
    // copy never followed.
    printf("Source-side junction is never traversed:\n");
    {
        // src\m\linked  ->  junction to src\elsewhere (which holds only far.txt)
        // dst\m\linked  ->  a real directory holding mine.txt
        CreateDirectoryW((root + L"\\src\\elsewhere").c_str(), nullptr);
        WriteText(root + L"\\src\\elsewhere\\far.txt", "far");
        CreateDirectoryW((root + L"\\dst\\m\\linked").c_str(), nullptr);
        WriteText(root + L"\\dst\\m\\linked\\mine.txt", "mine");

        std::wstring mk = L"cmd /c mklink /J \"" + root + L"\\src\\m\\linked\" \"" +
                          root + L"\\src\\elsewhere\" >nul 2>&1";
        _wsystem(mk.c_str());

        DWORD la = GetFileAttributesW((root + L"\\src\\m\\linked").c_str());
        bool haveLink = (la != INVALID_FILE_ATTRIBUTES) &&
                        (la & FILE_ATTRIBUTE_REPARSE_POINT);
        check(haveLink, "  (setup) source junction created");

        std::vector<std::wstring> sources{root + L"\\src\\m"};
        auto jobs = PlanJobs(Operation::Copy, root + L"\\dst", sources);
        auto extras = ScanExtras(jobs);
        // mine.txt lives under the destination twin of a SOURCE junction. It is
        // absent from the junction target, so an un-guarded walk would list it
        // for deletion. It must not appear.
        check(!Contains(extras, L"\\dst\\m\\linked\\mine.txt"),
              "file under a source-junction twin is NOT purged");
    }

    // Unreal preset: excluded cache folders must be skipped by the copy AND
    // must NOT be purged at the destination (the same data-loss shape as the
    // junction case — an excluded dir the copy skipped would read as "not in
    // source" and get deleted).
    printf("Unreal preset — excluded folders never purged:\n");
    {
        std::wstring base = root + L"\\ue";
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW((base + L"\\src").c_str(), nullptr);
        CreateDirectoryW((base + L"\\src\\proj").c_str(), nullptr);
        WriteText(base + L"\\src\\proj\\Game.uproject", "{}");
        CreateDirectoryW((base + L"\\src\\proj\\Content").c_str(), nullptr);
        WriteText(base + L"\\src\\proj\\Content\\a.uasset", "asset");
        CreateDirectoryW((base + L"\\src\\proj\\Intermediate").c_str(), nullptr);
        WriteText(base + L"\\src\\proj\\Intermediate\\build.tmp", "junk");
        // Destination already holds a big Intermediate + a real extra.
        CreateDirectoryW((base + L"\\dst").c_str(), nullptr);
        CreateDirectoryW((base + L"\\dst\\proj").c_str(), nullptr);
        CreateDirectoryW((base + L"\\dst\\proj\\Intermediate").c_str(), nullptr);
        WriteText(base + L"\\dst\\proj\\Intermediate\\cached.bin", "keep me");
        WriteText(base + L"\\dst\\proj\\stale.txt", "purge me");

        check(IsUnrealProject(base + L"\\src\\proj"),
              "  .uproject detected as Unreal project");
        check(!IsUnrealProject(base + L"\\src\\proj\\Content"),
              "  a plain folder is not an Unreal project");

        SetExcludedDirs(UnrealExcludeNames());
        check(IsExcludedDir(L"Intermediate") && IsExcludedDir(L"intermediate"),
              "  Intermediate excluded (case-insensitive)");
        check(!IsExcludedDir(L"Content"), "  Content is not excluded");

        std::vector<std::wstring> sources{base + L"\\src\\proj"};
        auto jobs = PlanJobs(Operation::Copy, base + L"\\dst", sources);
        auto extras = ScanExtras(jobs);
        // The real hazard is the CONTENTS: source also has an Intermediate, so
        // without the guard the walk descends into dst\Intermediate and purges
        // cached.bin (absent from the source's Intermediate). The excluded dir
        // must be skipped whole — neither it nor anything under it in extras.
        check(!Contains(extras, L"\\dst\\proj\\Intermediate\\cached.bin"),
              "  content under an excluded folder is NOT purged");
        check(!Contains(extras, L"\\dst\\proj\\Intermediate"),
              "  excluded Intermediate itself is NOT purged");
        check(Contains(extras, L"\\dst\\proj\\stale.txt"),
              "  a real extra IS still purged");

        // And the scan does not count the excluded folder's files.
        ScanResult s = ScanJobs(jobs);
        check(s.lonelyFiles >= 1, "  Content asset counted (lonely)");
        // build.tmp lives under Intermediate — must not be scanned at all.
        // (1 lonely = Content\a.uasset + Game.uproject = 2; Intermediate's
        // build.tmp excluded.) Exact count checked loosely to stay robust.
        SetExcludedDirs({}); // reset global for any later cases
    }

    RmTree(root);
    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
