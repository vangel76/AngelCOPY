#include "Robocopy.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <map>

#pragma comment(lib, "Shlwapi.lib")

namespace angelcopy {

namespace {

std::wstring StripTrailingSep(std::wstring p) {
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) {
        // keep a root like "C:\" intact
        if (p.size() == 3 && p[1] == L':') break;
        p.pop_back();
    }
    return p;
}

std::wstring ParentDir(const std::wstring& path) {
    std::wstring p = StripTrailingSep(path);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return p;
    if (slash == 2 && p[1] == L':') return p.substr(0, 3); // "C:\"
    return p.substr(0, slash);
}

std::wstring BaseName(const std::wstring& path) {
    std::wstring p = StripTrailingSep(path);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
}

bool IsDirectory(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Wrap in quotes for the command line. A robocopy path argument must not end in
// a backslash directly before the closing quote (the backslash escapes the
// quote), so a bare-root path like "C:\" is passed as "C:\\".
std::wstring Quote(std::wstring s) {
    if (!s.empty() && s.back() == L'\\') s.push_back(L'\\');
    return L"\"" + s + L"\"";
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    std::wstring d = StripTrailingSep(dir);
    return d + L"\\" + name;
}

std::wstring LowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

} // namespace

std::vector<RoboJob> PlanJobs(Operation /*op*/,
                              const std::wstring& destDir,
                              const std::vector<std::wstring>& sources) {
    std::vector<RoboJob> jobs;
    // parent-dir(lowercased) -> index into `jobs` for grouped loose files
    std::map<std::wstring, size_t> fileGroups;

    for (const auto& raw : sources) {
        std::wstring src = StripTrailingSep(raw);
        if (src.empty()) continue;

        if (IsDirectory(src)) {
            // Whole-tree copy: C:\a\sub  ->  dest\sub
            RoboJob job;
            job.srcDir = src;
            job.dstDir = JoinPath(destDir, BaseName(src));
            jobs.push_back(std::move(job));
        } else {
            // Loose file: group by parent directory so files sharing a folder
            // become one robocopy call with multiple file filters.
            std::wstring parent = ParentDir(src);
            std::wstring key = LowerCopy(parent);
            auto it = fileGroups.find(key);
            if (it == fileGroups.end()) {
                RoboJob job;
                job.srcDir = parent;
                job.dstDir = StripTrailingSep(destDir);
                job.files.push_back(BaseName(src));
                fileGroups[key] = jobs.size();
                jobs.push_back(std::move(job));
            } else {
                jobs[it->second].files.push_back(BaseName(src));
            }
        }
    }
    return jobs;
}

std::wstring BuildRobocopyArgs(Operation op, const RoboJob& job, bool parseable,
                               Conflict policy) {
    std::wstring args;
    args += Quote(job.srcDir);
    args += L" ";
    args += Quote(job.dstDir);

    for (const auto& f : job.files) {
        args += L" ";
        args += Quote(f);
    }

    // /MT:64  -> 64-thread multithreaded copy (the whole point of this tool)
    args += L" /MT:64";

    if (job.files.empty()) {
        // Whole-tree folder copy.
        args += L" /E";                 // subdirs incl. empty ones
        if (op == Operation::Move) args += L" /MOVE"; // move files + dirs
    } else {
        // Loose-file copy; only files, no /E.
        if (op == Operation::Move) args += L" /MOV";  // move files only
    }

    args += L" /COPY:DAT";   // data + attributes + timestamps (no owner/ACL: no admin needed)
    args += L" /R:2 /W:2";   // cap retries/wait so a locked file can't hang forever
    args += L" /XJ";         // skip junctions (avoid symlink loops)

    // Conflict handling. robocopy's default overwrites anything that differs —
    // including overwriting a NEWER destination with an OLDER source — so the
    // non-default policies exist to make that survivable.
    switch (policy) {
    case Conflict::Skip:
        args += L" /XC /XN /XO"; // exclude changed/newer/older -> only new files
        break;
    case Conflict::ReplaceIfNewer:
        args += L" /XO";         // exclude older source files
        break;
    case Conflict::Replace:
        break;                   // default: overwrite everything that differs
    }

    if (parseable) {
        // Machine-readable output for the progress UI: one <bytes>\t<fullpath>
        // line per file, no headers/summary/dir-list. /NP is important: it drops
        // the per-file percentage stream, which otherwise floods the pipe and
        // throttles robocopy's copy threads when the reader can't keep up.
        // Errors still print regardless of these flags.
        //
        // /V makes robocopy list SKIPPED files too (same + policy-excluded),
        // which is what lets the progress UI advance and paint those stretches
        // green. Without it skipped files are invisible in the output. Measured
        // cost: ~0.17 ms per skipped file (4000 all-skip: 80 ms -> 880 ms;
        // 12000: 116 ms -> 2155 ms) — linear, and only skips pay it.
        args += L" /BYTES /FP /NC /NDL /NJH /NJS /NP /V";
    } else {
        args += L" /NP /NDL"; // quiet-ish console
    }
    return args;
}

std::wstring RobocopyExe() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return (n && n < MAX_PATH) ? std::wstring(sys) + L"\\robocopy.exe"
                              : L"robocopy.exe";
}

namespace {

constexpr size_t kConflictSampleMax = 500;

unsigned long long FileTimeToU64(const FILETIME& ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// How robocopy will treat a source file given what's at the destination.
enum class FileClass { Lonely, Same, DiffNewer, DiffOlder };

constexpr unsigned long long kTimeTolerance = 20000000ULL; // 2s in 100ns ticks

// Mirrors robocopy's own comparison: identical == same size and write-time
// within 2s (FAT/network timestamp granularity).
FileClass Classify(const std::wstring& dst, unsigned long long srcSize,
                   const FILETIME& srcTime) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fa))
        return FileClass::Lonely;

    unsigned long long dstSize =
        ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long a = FileTimeToU64(srcTime);
    unsigned long long b = FileTimeToU64(fa.ftLastWriteTime);
    unsigned long long diff = (a > b) ? a - b : b - a;

    if (dstSize == srcSize && diff <= kTimeTolerance) return FileClass::Same;
    // Same timestamp but different size is robocopy's "changed": copied by
    // default and not excluded by /XO, so it belongs with the newer bucket.
    if (b > a && diff > kTimeTolerance) return FileClass::DiffOlder;
    return FileClass::DiffNewer;
}

void Account(ScanResult& acc, FileClass fc, unsigned long long size,
             const std::wstring& src, const std::wstring& dst) {
    switch (fc) {
    case FileClass::Lonely:
        acc.lonelyFiles++; acc.lonelyBytes += size;
        return;
    case FileClass::Same:
        acc.sameFiles++;   acc.sameBytes += size;
        acc.samePaths.push_back(LowerCopy(src));
        return;
    case FileClass::DiffNewer:
        acc.newerFiles++;  acc.newerBytes += size;
        acc.newerPaths.push_back(LowerCopy(src));
        break;
    case FileClass::DiffOlder:
        acc.olderFiles++;  acc.olderBytes += size;
        acc.olderPaths.push_back(LowerCopy(src));
        break;
    }
    acc.conflicts++;
    if (acc.conflictSample.size() < kConflictSampleMax)
        acc.conflictSample.push_back(dst);
}

void ScanTree(const std::wstring& srcDir, const std::wstring& dstDir,
              ScanResult& acc) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((srcDir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // /XJ

        std::wstring src = srcDir + L"\\" + fd.cFileName;
        std::wstring dst = dstDir + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanTree(src, dst, acc);
        } else {
            unsigned long long size =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            Account(acc, Classify(dst, size, fd.ftLastWriteTime), size, src, dst);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

ScanResult ScanJobs(const std::vector<RoboJob>& jobs) {
    ScanResult r;
    for (const RoboJob& job : jobs) {
        if (job.files.empty()) {
            // Whole-tree job: srcDir maps onto dstDir.
            ScanTree(job.srcDir, job.dstDir, r);
        } else {
            for (const auto& f : job.files) {
                std::wstring src = job.srcDir + L"\\" + f;
                std::wstring dst = job.dstDir + L"\\" + f;
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa))
                    continue;
                unsigned long long size =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                Account(r, Classify(dst, size, fa.ftLastWriteTime), size, src, dst);
            }
        }
    }
    return r;
}

void ExpectedFor(const ScanResult& s, Conflict policy,
                 unsigned long long& bytes, unsigned long long& files) {
    // "Same" files are never copied under any policy, so they never count.
    bytes = s.lonelyBytes;
    files = s.lonelyFiles;
    if (policy == Conflict::Replace || policy == Conflict::ReplaceIfNewer) {
        bytes += s.newerBytes;
        files += s.newerFiles;
    }
    if (policy == Conflict::Replace) { // /XO would exclude these
        bytes += s.olderBytes;
        files += s.olderFiles;
    }
}

namespace {

// Walk one destination directory; anything not present in the source (or
// present with a different type) is an extra. Recurse only where BOTH sides
// are real directories — a reparse point at the destination is either an
// extra (deleted as a link) or left alone, never entered.
void FindExtras(const std::wstring& srcDir, const std::wstring& dstDir,
                std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dstDir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring dst = dstDir + L"\\" + fd.cFileName;
        std::wstring src = srcDir + L"\\" + fd.cFileName;
        bool dstIsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool dstIsLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        DWORD sa = GetFileAttributesW(src.c_str());
        if (sa == INVALID_FILE_ATTRIBUTES) {
            out.push_back(dst); // not in the source at all
            continue;
        }
        bool srcIsDir = (sa & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (srcIsDir != dstIsDir) {
            // Type mismatch: the copy phase can only recreate it if the old
            // entry is gone first.
            out.push_back(dst);
            continue;
        }
        if (dstIsDir && !dstIsLink) FindExtras(src, dst, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

std::vector<std::wstring> ScanExtras(const std::vector<RoboJob>& jobs) {
    std::vector<std::wstring> extras;
    for (const RoboJob& job : jobs) {
        if (!job.files.empty()) continue; // loose files: plain copy, no purge
        if (!IsDirectory(job.dstDir)) continue; // nothing there yet
        FindExtras(job.srcDir, job.dstDir, extras);
    }
    return extras;
}

std::unordered_set<std::wstring> SkipSetFor(const ScanResult& s, Conflict policy) {
    // Must mirror SkippedFor exactly: same files always skip; the policy adds
    // its excluded classes. Under Replace, newer/older files ARE copied and
    // must not be in the set, or real copies would be painted as skips.
    std::unordered_set<std::wstring> set(s.samePaths.begin(), s.samePaths.end());
    switch (policy) {
    case Conflict::Replace:
        break;
    case Conflict::ReplaceIfNewer:
        set.insert(s.olderPaths.begin(), s.olderPaths.end());
        break;
    case Conflict::Skip:
        set.insert(s.newerPaths.begin(), s.newerPaths.end());
        set.insert(s.olderPaths.begin(), s.olderPaths.end());
        break;
    }
    return set;
}

SkipInfo SkippedFor(const ScanResult& s, Conflict policy) {
    SkipInfo k;
    // Identical files are skipped by robocopy under every policy.
    k.identicalFiles = s.sameFiles;
    k.identicalBytes = s.sameBytes;

    switch (policy) {
    case Conflict::Replace:
        break; // nothing else skipped
    case Conflict::ReplaceIfNewer: // /XO drops older sources
        k.policyFiles = s.olderFiles;
        k.policyBytes = s.olderBytes;
        break;
    case Conflict::Skip: // /XC /XN /XO drops everything already there
        k.policyFiles = s.newerFiles + s.olderFiles;
        k.policyBytes = s.newerBytes + s.olderBytes;
        break;
    }
    return k;
}

int RunJobs(Operation op, const std::vector<RoboJob>& jobs, Conflict policy) {
    std::wstring robocopy = RobocopyExe();

    int worst = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const RoboJob& job = jobs[i];
        std::wstring args = BuildRobocopyArgs(op, job, /*parseable=*/false, policy);

        std::wstring cmd = Quote(robocopy) + L" " + args;

        wprintf(L"\n[AngelCOPY] %s  ->  %s\n",
                job.srcDir.c_str(), job.dstDir.c_str());

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');

        BOOL ok = CreateProcessW(robocopy.c_str(), mutableCmd.data(), nullptr,
                                 nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
        if (!ok) {
            wprintf(L"[AngelCOPY] failed to launch robocopy (err %lu)\n",
                    GetLastError());
            worst = 16;
            continue;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (static_cast<int>(code) > worst) worst = static_cast<int>(code);
    }
    return worst;
}

} // namespace angelcopy
