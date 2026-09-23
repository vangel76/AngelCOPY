#include "Robocopy.h"
#include "BatchQueue.h"
#include "../shared/Util.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <map>
#include <thread>

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

// Ext() below is declared after this point; forward-declare so the long-path
// form is used here too. A >MAX_PATH source directory that read as "not a
// directory" was planned as a loose-file job and then copied nothing.
std::wstring Ext(const std::wstring& p);

bool IsDirectory(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(Ext(path).c_str());
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

// \\?\ long-path form for the scan/enumeration side, so a source subtree past
// MAX_PATH is walked and classified exactly like the native engine copies it.
// Without this, FindFirstFile on a >MAX_PATH path fails and the scan silently
// omits those files — a copy reported complete but short, and (worse) in a
// mirror an existing source read as "missing" so its destination twin was
// purged. Canonical implementation: acutil::ExtLongPath (shared\Util.h).
std::wstring Ext(const std::wstring& p) { return acutil::ExtLongPath(p); }

std::wstring LowerCopy(std::wstring s) { return acutil::LowerCopy(std::move(s)); }

// Set once in main before any scan/transfer, read-only thereafter from every
// walk thread (see Robocopy.h). Lowercased directory names.
std::unordered_set<std::wstring> g_excludedDirs;

// Whether Account() collects the per-file skip-path lists (samePaths etc.).
// Only the robocopy fallback engine reads the SkipSetFor set built from them
// (pipe-line matching); the native engine reports skips itself. Defaults ON
// so the unit tests and the fallback keep their lists; main.cpp switches it
// off for native runs — on a 500k-file re-mirror the lists were ~1M string
// allocations pinned for the whole transfer, feeding nothing.
bool g_collectSkipPaths = true;

// Whether Account() collects per-file verdicts into ScanResult::classes for
// the engine carry (default OFF: tests and the robocopy fallback don't use
// it). Set between the confirmation and the run, read concurrently by the
// copy pool afterwards — same lifecycle contract as g_excludedDirs.
bool g_collectClasses = false;
std::unordered_map<std::wstring, FileClass> g_carriedClasses;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return LowerCopy(StripTrailingSep(a)) == LowerCopy(StripTrailingSep(b));
}

bool Exists(const std::wstring& p) {
    // Long-path form: UniqueCopyName uses this to find a free "<name> - Kopie";
    // a bare query on a >MAX_PATH candidate reads as free and the copy then
    // OVERWRITES the existing one.
    return GetFileAttributesW(Ext(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Windows-style "<stem> - <copyWord><ext>", bumped to " (2)", " (3)" … until
// the name is free in `dir`. For files the extension is the last dot (so
// "a.tar.gz" -> "a.tar - Kopie.gz", matching Explorer); folders have none.
std::wstring UniqueCopyName(const std::wstring& dir, const std::wstring& name,
                            bool isDir, const std::wstring& copyWord) {
    std::wstring stem = name, ext;
    if (!isDir) {
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot != 0) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
    }
    for (int n = 1;; ++n) {
        std::wstring cand = stem + L" - " + copyWord;
        if (n > 1) cand += L" (" + std::to_wstring(n) + L")";
        cand += ext;
        if (!Exists(JoinPath(dir, cand))) return cand;
    }
}

} // namespace

std::vector<RoboJob> PlanJobs(Operation op,
                              const std::wstring& destDir,
                              const std::vector<std::wstring>& sources,
                              const std::wstring& copyWord) {
    std::vector<RoboJob> jobs;
    // parent-dir(lowercased) -> index into `jobs` for grouped loose files
    std::map<std::wstring, size_t> fileGroups;
    const bool isCopy = (op == Operation::Copy);

    for (const auto& raw : sources) {
        std::wstring src = StripTrailingSep(raw);
        if (src.empty()) continue;

        if (IsDirectory(src)) {
            // Whole-tree copy: C:\a\sub  ->  dest\sub
            RoboJob job;
            job.srcDir = src;
            std::wstring dst = JoinPath(destDir, BaseName(src));
            if (SamePath(dst, src)) {
                // Pasting a folder into its own parent. A move onto itself is a
                // no-op — drop it; a copy becomes "<name> - Kopie".
                if (!isCopy) continue;
                dst = JoinPath(destDir,
                               UniqueCopyName(destDir, BaseName(src), true, copyWord));
            }
            job.dstDir = std::move(dst);
            jobs.push_back(std::move(job));
        } else {
            std::wstring parent = ParentDir(src);
            std::wstring dstDir = StripTrailingSep(destDir);
            std::wstring name = BaseName(src);

            if (SamePath(parent, dstDir)) {
                // Same-folder paste of a loose file. Move onto itself: nothing
                // to do. Copy: emit an own job with a renamed destination.
                if (!isCopy) continue;
                RoboJob job;
                job.srcDir = parent;
                job.dstDir = dstDir;
                job.files.push_back(name);
                job.dstNames.push_back(
                    UniqueCopyName(dstDir, name, false, copyWord));
                jobs.push_back(std::move(job));
                continue;
            }

            // Different folder: group by parent directory so files sharing a
            // folder become one job with multiple file filters.
            std::wstring key = LowerCopy(parent);
            auto it = fileGroups.find(key);
            if (it == fileGroups.end()) {
                RoboJob job;
                job.srcDir = parent;
                job.dstDir = dstDir;
                job.files.push_back(name);
                fileGroups[key] = jobs.size();
                jobs.push_back(std::move(job));
            } else {
                jobs[it->second].files.push_back(name);
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

    // /XD: excluded folder names (Unreal preset). Whole-tree jobs only — a
    // loose-file job has no subdirs to exclude. robocopy /XD is case-insensitive
    // and excludes from the purge too, matching the native walk's behavior.
    if (job.files.empty() && AnyExcludedDirs()) {
        // Emit the ACTUAL exclusion state, not UnrealExcludeNames(): the
        // fallback engine must not diverge from the native walks if the set
        // ever comes from somewhere else. (Lowercased is fine — /XD is
        // case-insensitive.)
        args += L" /XD";
        for (const auto& n : ExcludedDirNames()) { args += L" "; args += Quote(n); }
    }

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

constexpr unsigned long long kTimeTolerance = 20000000ULL; // 2s in 100ns ticks

} // namespace

// Definition lives in Robocopy.cpp next to the scan that depends on it; the
// declaration is public (Robocopy.h) for the native engine.
FileClass ClassifyFile(const std::wstring& dst, unsigned long long srcSize,
                       const FILETIME& srcTime, unsigned long long& dstSize) {
    dstSize = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(Ext(dst).c_str(), GetFileExInfoStandard, &fa))
        return FileClass::Lonely;

    dstSize = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long a = FileTimeToU64(srcTime);
    unsigned long long b = FileTimeToU64(fa.ftLastWriteTime);
    unsigned long long diff = (a > b) ? a - b : b - a;

    if (dstSize == srcSize && diff <= kTimeTolerance) return FileClass::Same;
    // Same timestamp but different size is robocopy's "changed": copied by
    // default and not excluded by /XO, so it belongs with the newer bucket.
    if (b > a && diff > kTimeTolerance) return FileClass::DiffOlder;
    return FileClass::DiffNewer;
}

namespace {

void CollectClass(ScanResult& acc, FileClass fc, const std::wstring& src) {
    // Fast-path Lonely entries arrive with an empty src (no strings built);
    // the engine's dstFresh shortcut covers those statlessly anyway.
    if (g_collectClasses && !src.empty())
        acc.classes.emplace(LowerCopy(src), fc);
}

void Account(ScanResult& acc, FileClass fc, unsigned long long size,
             unsigned long long dstSize, const std::wstring& src,
             const std::wstring& dst) {
    unsigned long long grow = (size > dstSize) ? size - dstSize : 0;
    CollectClass(acc, fc, src);
    switch (fc) {
    case FileClass::Lonely:
        acc.lonelyFiles++; acc.lonelyBytes += size;
        return;
    case FileClass::Same:
        acc.sameFiles++;   acc.sameBytes += size;
        if (g_collectSkipPaths) acc.samePaths.push_back(LowerCopy(src));
        return;
    case FileClass::DiffNewer:
        acc.newerFiles++;  acc.newerBytes += size;
        acc.newerGrowBytes += grow;
        if (g_collectSkipPaths) acc.newerPaths.push_back(LowerCopy(src));
        break;
    case FileClass::DiffOlder:
        acc.olderFiles++;  acc.olderBytes += size;
        acc.olderGrowBytes += grow;
        if (g_collectSkipPaths) acc.olderPaths.push_back(LowerCopy(src));
        break;
    }
    acc.conflicts++;
    if (acc.conflictSample.size() < kConflictSampleMax)
        acc.conflictSample.push_back(dst);
}

// Cap recursion so a pathologically deep tree can't overflow the stack. The
// scan just stops descending at the cap; the copy walk (NativeCopy) caps
// identically, so totals and copy stay consistent.
constexpr int kMaxScanDepth = 900;

// (The former single-threaded ScanTree lives on as WalkForScan below: same
// walk, but the expensive per-file destination stats fan out to a pool.)

} // namespace

// ---- parallel destination classification ----------------------------------
// The scan's cost is NOT the source enumeration — it is the one destination
// stat per file (ClassifyFile), a latency-bound metadata round-trip that on a
// slow USB/SMB target dominates "Preparing" entirely. Latency parallelizes:
// the delete pool measured 3.5x with 8 threads on exactly this kind of load.
// So the walk (producer) stays single-threaded and cheap, while the per-file
// classification fans out to a small pool. Each worker accumulates into its
// own ScanResult; results merge at the end (order inside conflictSample is
// arbitrary anyway). 8 threads mirrors kDelThreads — same bound, same disk
// behavior (re-measure per target before changing, as ever).

namespace {

constexpr int kScanThreads = 8;

struct ScanFileItem {
    std::wstring src, dst;
    unsigned long long size;
    FILETIME mtime;
};

using ScanQueue = BatchQueue<ScanFileItem>;

// Same safety net as the copy/delete chunk split: without a cap one flat
// 100k-file directory would land as ONE batch on ONE of the 8 workers while
// the other 7 idle — on exactly the latency-bound load this pool exists for.
constexpr size_t kScanBatchFiles = 256;

// Walk one tree, pushing per-directory batches of files that still need the
// destination stat. Files under a missing destination dir are accounted
// directly as Lonely (no stat needed, no batch).
void WalkForScan(const std::wstring& srcDir, const std::wstring& dstDir,
                 ScanResult& lonelyAcc, ScanProgress* prog, ScanQueue& queue,
                 int depth, bool dstExists) {
    if (depth >= kMaxScanDepth) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Ext(srcDir + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<ScanFileItem> batch;
    auto flush = [&] {
        if (!batch.empty()) { queue.Push(std::move(batch)); batch = {}; }
    };
    do {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // /XJ
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            IsExcludedDir(fd.cFileName))
            continue; // /XD

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring src = srcDir + L"\\" + fd.cFileName;
            std::wstring dst = dstDir + L"\\" + fd.cFileName;
            bool childDst = dstExists && IsDirectory(dst);
            WalkForScan(src, dst, lonelyAcc, prog, queue, depth + 1, childDst);
        } else {
            if (prog) prog->files.fetch_add(1, std::memory_order_relaxed);
            unsigned long long size =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!dstExists) {
                // Lonely by construction: Account uses neither path for this
                // class, so the fast path builds no strings at all.
                Account(lonelyAcc, FileClass::Lonely, size, 0, L"", L"");
            } else {
                batch.push_back(ScanFileItem{srcDir + L"\\" + fd.cFileName,
                                             dstDir + L"\\" + fd.cFileName,
                                             size, fd.ftLastWriteTime});
                if (batch.size() >= kScanBatchFiles) flush();
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    flush();
}

void MergeScan(ScanResult& into, ScanResult& from) {
    into.lonelyBytes += from.lonelyBytes;   into.lonelyFiles += from.lonelyFiles;
    into.sameBytes += from.sameBytes;       into.sameFiles += from.sameFiles;
    into.newerBytes += from.newerBytes;     into.newerFiles += from.newerFiles;
    into.olderBytes += from.olderBytes;     into.olderFiles += from.olderFiles;
    into.newerGrowBytes += from.newerGrowBytes;
    into.olderGrowBytes += from.olderGrowBytes;
    into.conflicts += from.conflicts;
    for (auto& s : from.conflictSample)
        if (into.conflictSample.size() < kConflictSampleMax)
            into.conflictSample.push_back(std::move(s));
    auto app = [](std::vector<std::wstring>& a, std::vector<std::wstring>& b) {
        a.insert(a.end(), std::make_move_iterator(b.begin()),
                 std::make_move_iterator(b.end()));
    };
    app(into.samePaths, from.samePaths);
    app(into.newerPaths, from.newerPaths);
    app(into.olderPaths, from.olderPaths);
    if (into.classes.empty()) into.classes = std::move(from.classes);
    else into.classes.merge(from.classes);
}

} // namespace

ScanResult ScanJobs(const std::vector<RoboJob>& jobs, ScanProgress* prog) {
    ScanResult r;
    // Pool + queue live across all whole-tree jobs so one warm-up serves all.
    ScanQueue queue;
    std::vector<ScanResult> local(kScanThreads);
    std::vector<std::thread> pool;
    for (int t = 0; t < kScanThreads; ++t)
        pool.emplace_back([&queue, &local, t, prog] {
            std::vector<ScanFileItem> batch;
            while (queue.Pop(batch))
                for (auto& it : batch) {
                    if (prog && prog->cancel.load(std::memory_order_relaxed))
                        return;
                    unsigned long long dstSize = 0;
                    FileClass fc =
                        ClassifyFile(it.dst, it.size, it.mtime, dstSize);
                    Account(local[t], fc, it.size, dstSize, it.src, it.dst);
                }
        });

    for (const RoboJob& job : jobs) {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (job.files.empty()) {
            // Whole-tree job: srcDir maps onto dstDir. If the destination root
            // doesn't exist (fresh backup), the whole tree is Lonely and every
            // per-file destination stat is skipped — the scan collapses to a
            // source-only walk, which is what makes "Preparing" fast on a slow
            // target. Otherwise the stats fan out to the pool above.
            WalkForScan(job.srcDir, job.dstDir, r, prog, queue, 0,
                        IsDirectory(job.dstDir));
        } else {
            for (size_t i = 0; i < job.files.size(); ++i) {
                const std::wstring& f = job.files[i];
                if (prog) prog->files.fetch_add(1, std::memory_order_relaxed);
                std::wstring src = job.srcDir + L"\\" + f;
                std::wstring dst = job.dstDir + L"\\" +
                    (i < job.dstNames.size() ? job.dstNames[i] : f);
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (!GetFileAttributesExW(Ext(src).c_str(), GetFileExInfoStandard, &fa))
                    continue;
                unsigned long long size =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                unsigned long long dstSize = 0;
                FileClass fc = ClassifyFile(dst, size, fa.ftLastWriteTime, dstSize);
                Account(r, fc, size, dstSize, src, dst);
            }
        }
    }

    queue.Close();
    for (auto& t : pool) t.join();
    for (auto& l : local) MergeScan(r, l);
    return r;
}

bool PolicyCopies(Conflict policy, FileClass fc) {
    switch (fc) {
    case FileClass::Lonely:    return true;  // nothing at the destination
    case FileClass::Same:      return false; // identical is never copied
    case FileClass::DiffNewer: return policy != Conflict::Skip;
    case FileClass::DiffOlder: return policy == Conflict::Replace;
    }
    return true;
}

namespace {

// The four class buckets of a ScanResult, so the aggregates below iterate
// against PolicyCopies instead of each re-encoding the policy by hand.
struct ClassBucket {
    FileClass fc;
    unsigned long long files, bytes;
    const std::vector<std::wstring>* paths; // null for Lonely (never skipped)
};

std::array<ClassBucket, 4> BucketsOf(const ScanResult& s) {
    return {{{FileClass::Lonely, s.lonelyFiles, s.lonelyBytes, nullptr},
             {FileClass::Same, s.sameFiles, s.sameBytes, &s.samePaths},
             {FileClass::DiffNewer, s.newerFiles, s.newerBytes, &s.newerPaths},
             {FileClass::DiffOlder, s.olderFiles, s.olderBytes, &s.olderPaths}}};
}

} // namespace

void ExpectedFor(const ScanResult& s, Conflict policy,
                 unsigned long long& bytes, unsigned long long& files) {
    bytes = files = 0;
    for (const auto& b : BucketsOf(s))
        if (PolicyCopies(policy, b.fc)) { bytes += b.bytes; files += b.files; }
}

unsigned long long NeededSpaceFor(const ScanResult& s, Conflict policy) {
    // Lonely files cost their full size; overwrites cost only their GROWTH
    // (in-place, measured) — deliberately not a plain bucket sum. Which
    // classes are overwritten at all still comes from the shared predicate.
    unsigned long long need = s.lonelyBytes;
    if (PolicyCopies(policy, FileClass::DiffNewer)) need += s.newerGrowBytes;
    if (PolicyCopies(policy, FileClass::DiffOlder)) need += s.olderGrowBytes;
    return need;
}

namespace {

// Walk one destination directory; anything not present in the source (or
// present with a different type) is an extra. Recurse only where BOTH sides
// are real directories — a reparse point at the destination is either an
// extra (deleted as a link) or left alone, never entered.
void FindExtras(const std::wstring& srcDir, const std::wstring& dstDir,
                std::vector<std::wstring>& out, ScanProgress* prog,
                int depth = 0) {
    // At the depth cap, stop descending WITHOUT emitting extras: an unscanned
    // level must never have its destination entries deleted as "missing".
    if (depth >= kMaxScanDepth) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Ext(dstDir + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        // /XD safety: an excluded folder at the destination is neither purged
        // nor entered — the copy skipped it, so treating it as "not in source"
        // would delete the cache the user asked to keep.
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            IsExcludedDir(fd.cFileName))
            continue;
        if (prog && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            prog->files.fetch_add(1, std::memory_order_relaxed);

        std::wstring dst = dstDir + L"\\" + fd.cFileName;
        std::wstring src = srcDir + L"\\" + fd.cFileName;
        bool dstIsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool dstIsLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        // MUST be \\?\-prefixed: a long src path reading as INVALID here would
        // mark an existing source entry as "extra" and DELETE its destination
        // twin during the purge — destination data loss in the safest op.
        DWORD sa = GetFileAttributesW(Ext(src).c_str());
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
        // A SOURCE-side reparse point must not be traversed either. The copy
        // phase skips source junctions entirely (ScanTree/WalkStream `continue`
        // on them), so descending here would compare the destination against
        // the LINK TARGET's contents and purge everything that isn't in it —
        // destination data loss driven by a link the copy never followed.
        if (sa & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (dstIsDir && !dstIsLink) FindExtras(src, dst, out, prog, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

std::vector<std::wstring> ScanExtras(const std::vector<RoboJob>& jobs,
                                     ScanProgress* prog) {
    std::vector<std::wstring> extras;
    for (const RoboJob& job : jobs) {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (!job.files.empty()) continue; // loose files: plain copy, no purge
        if (!IsDirectory(job.dstDir)) continue; // nothing there yet
        FindExtras(job.srcDir, job.dstDir, extras, prog);
    }
    return extras;
}

std::unordered_set<std::wstring> SkipSetFor(const ScanResult& s, Conflict policy) {
    // Exactly the paths of every class the policy does NOT copy — under
    // Replace, newer/older files ARE copied and must not be in the set, or
    // real copies would be painted as skips. Same predicate as SkippedFor by
    // construction.
    std::unordered_set<std::wstring> set;
    for (const auto& b : BucketsOf(s))
        if (!PolicyCopies(policy, b.fc) && b.paths)
            set.insert(b.paths->begin(), b.paths->end());
    return set;
}

SkipInfo SkippedFor(const ScanResult& s, Conflict policy) {
    SkipInfo k;
    for (const auto& b : BucketsOf(s)) {
        if (PolicyCopies(policy, b.fc)) continue;
        if (b.fc == FileClass::Same) {
            // Identical files are skipped under every policy — reported as
            // their own line ("already up to date"), not as a policy skip.
            k.identicalFiles = b.files;
            k.identicalBytes = b.bytes;
        } else {
            k.policyFiles += b.files;
            k.policyBytes += b.bytes;
        }
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

// ---- directory exclusions (Unreal preset) --------------------------------

void SetExcludedDirs(const std::vector<std::wstring>& names) {
    g_excludedDirs.clear();
    for (const auto& n : names) g_excludedDirs.insert(LowerCopy(n));
}

void SetCollectSkipPaths(bool on) { g_collectSkipPaths = on; }

void SetCollectClasses(bool on) { g_collectClasses = on; }

void SetCarriedClasses(std::unordered_map<std::wstring, FileClass>&& m) {
    // Memory cap: a huge tree's verdict map is discarded rather than pinned —
    // the engine then re-classifies streamed, exactly as before the carry.
    if (m.size() > kMaxCarriedClasses) m.clear();
    g_carriedClasses = std::move(m);
}

bool AnyCarriedClasses() { return !g_carriedClasses.empty(); }

bool LookupCarriedClass(const std::wstring& srcLower, FileClass& fc) {
    auto it = g_carriedClasses.find(srcLower);
    if (it == g_carriedClasses.end()) return false;
    fc = it->second;
    return true;
}

bool IsExcludedDir(const std::wstring& name) {
    return !g_excludedDirs.empty() && g_excludedDirs.count(LowerCopy(name)) > 0;
}

// find-data overload: the walks call this with fd.cFileName for EVERY
// directory entry; without it the wstring parameter forced a heap temporary
// before the empty() early-out could run — an allocation per directory in
// copy, scan and purge, even with no exclusions set (the common case).
bool IsExcludedDir(const wchar_t* name) {
    if (g_excludedDirs.empty()) return false;
    return IsExcludedDir(std::wstring(name));
}

bool AnyExcludedDirs() { return !g_excludedDirs.empty(); }

std::vector<std::wstring> ExcludedDirNames() {
    return {g_excludedDirs.begin(), g_excludedDirs.end()};
}

const std::vector<std::wstring>& UnrealExcludeNames() {
    // The four regenerable folders from the user's own robocopy /XD line.
    static const std::vector<std::wstring> names = {
        L"DerivedDataCache", L"Intermediate", L"Saved", L"Binaries"};
    return names;
}

bool IsUnrealProject(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Ext(dir + L"\\*.uproject").c_str(),
                               FindExInfoBasic, &fd, FindExSearchNameMatch,
                               nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { found = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

} // namespace angelcopy
