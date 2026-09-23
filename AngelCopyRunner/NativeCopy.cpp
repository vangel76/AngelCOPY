#include "NativeCopy.h"
#include "BatchQueue.h"
#include "../shared/Util.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace angelcopy {

namespace {

// Measured tuning (bench/bench.cpp). Do not change without re-measuring.
constexpr unsigned long long kBigFileBytes = 32ull << 20; // ring threshold
constexpr DWORD  kRingChunk = 8u << 20;                   // 8 MiB
constexpr int    kRingDepth = 8;                          // QD8
// Recursion cap: a pathologically deep source tree (\\?\ allows thousands of
// levels) must not overflow the ~1 MB stack. Matches Robocopy/Delete.
constexpr int    kMaxWalkDepth = 900;
constexpr size_t kChunkFiles = 256;  // dir-shard split so flat dirs parallelize
constexpr int    kRetries = 2;       // matches the old /R:2
constexpr DWORD  kRetryWaitMs = 2000; // matches the old /W:2

// 16 measured optimal on NVMe (more collapses on NTFS same-directory create
// contention). ANGELCOPY_THREADS overrides for future measurement (e.g. SMB).
int PoolThreads() {
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(L"ANGELCOPY_THREADS", buf, 16);
    if (n && n < 16) {
        int v = _wtoi(buf);
        if (v >= 1 && v <= 64) return v;
    }
    return 16;
}

// \\?\ lifts MAX_PATH for the actual I/O calls. Paths arrive absolute from
// the shell; UNC needs the \\?\UNC\ form. Canonical: acutil::ExtLongPath.
std::wstring ExtPath(const std::wstring& p) { return acutil::ExtLongPath(p); }

// Create every missing component of `dir` (robocopy creates the full
// destination path; the shell may hand us a target several levels deep).
void CreateDirDeep(const std::wstring& dir) {
    if (GetFileAttributesW(ExtPath(dir).c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    // Skip the volume root ("C:\") or the \\server\share of a UNC path.
    size_t start;
    if (dir.rfind(L"\\\\", 0) == 0) {
        size_t s1 = dir.find(L'\\', 2);
        size_t s2 = (s1 == std::wstring::npos) ? s1 : dir.find(L'\\', s1 + 1);
        if (s2 == std::wstring::npos) return;
        start = s2 + 1;
    } else {
        start = (dir.size() > 2 && dir[1] == L':') ? 3 : 0;
    }
    for (size_t i = start; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            if (i > start) CreateDirectoryW(ExtPath(dir.substr(0, i)).c_str(), nullptr);
        }
    }
}

std::wstring ParentOf(const std::wstring& p) {
    size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? p : p.substr(0, cut);
}

// Localized OS error text; canonical implementation in shared\Util.h.
std::wstring WinErrText(DWORD err) { return acutil::WinErrText(err); }

// Same language-neutral "(0xNNNNNNNN)" marker robocopy lines carried, so the
// report box looks familiar; the text itself is the OS's own localized string.
void ReportError(const CopySink& sink, const std::wstring& path, DWORD err) {
    if (!sink.onError) return;
    wchar_t code[32];
    swprintf(code, 32, L" (0x%08lX)  ", err);
    sink.onError(path + code + WinErrText(err));
}

bool Cancelled(const CopySink& sink) {
    return sink.cancelled && sink.cancelled();
}

// The engine's per-file decision IS the shared predicate (Robocopy.h) — the
// scan and the engine cannot drift apart when both read one definition.
bool PolicySkips(Conflict policy, FileClass fc) {
    return !PolicyCopies(policy, fc);
}

// Carried scan verdict, or a fresh destination stat on a miss. The carry
// (SetCarriedClasses, Robocopy.h) removes the copy phase's SECOND round of
// destination stats — on a skip-heavy mirror over a slow target those
// re-stats were the entire copy phase. A miss (file appeared or the map was
// discarded over the memory cap) stats for real, so carried verdicts are
// an optimization, never the only truth.
FileClass ClassifyOrCarried(const std::wstring& src, const std::wstring& dst,
                            unsigned long long size, const FILETIME& mtime) {
    FileClass fc = FileClass::Lonely; // overwritten on a lookup hit (C4701)
    if (AnyCarriedClasses() && LookupCarriedClass(acutil::LowerCopy(src), fc))
        return fc;
    unsigned long long dstSize = 0;
    return ClassifyFile(dst, size, mtime, dstSize);
}

struct Item {
    std::wstring src, dst;
    unsigned long long size = 0;
    // Deferred classification (see WalkStream): the pool worker stats the
    // destination instead of the walk thread, so a skip-heavy re-mirror
    // parallelizes its stats instead of serializing them on the walk.
    FILETIME mtime{};
    bool classify = false;
};

struct Plan {
    std::vector<Item> smallItems;   // < kBigFileBytes, in directory walk order
    std::vector<Item> big;     // copied one at a time via the overlapped ring
    std::vector<Item> skips;   // listed but not copied (green bar)
    // Source dirs post-order (deepest first), for move cleanup: after a move,
    // emptied source directories are removed bottom-up; RemoveDirectory fails
    // harmlessly on dirs still holding policy-skipped files.
    std::vector<std::wstring> srcDirsPostOrder;
};

// Streaming walker->pool queue; see BatchQueue.h for why it streams.
using ChunkQueue = BatchQueue<Item>;

// Streaming walk mirroring ScanTree exactly (reparse points skipped — the old
// /XJ — so outcomes match the scan's totals). Creates destination dirs on the
// way down (CreateDirectoryEx copies the source dir's attributes), reports
// skips immediately (the green bar advances during the walk), collects big
// files for the ring, and enqueues small files in per-directory chunks. A
// chunk is flushed before recursing into a subdirectory and at 256 files, so
// copying starts as soon as the first directory is read.
void WalkStream(const std::wstring& srcDir, const std::wstring& dstDir,
                Conflict policy, const CopySink& sink, ChunkQueue& queue,
                std::vector<Item>& bigs, std::vector<std::wstring>& dirsPost,
                int depth = 0) {
    if (Cancelled(sink)) return;
    if (depth >= kMaxWalkDepth) {
        ReportError(sink, srcDir, ERROR_STACK_OVERFLOW);
        return;
    }
    // `dstFresh`: this call CREATED the destination directory, so nothing can
    // exist inside it — every file below is Lonely by construction and the
    // per-file ClassifyFile destination stat (one round-trip per file; the
    // dominant cost on a slow USB/network target) is skipped.
    bool dstFresh = true;
    if (!CreateDirectoryExW(ExtPath(srcDir).c_str(), ExtPath(dstDir).c_str(),
                            nullptr)) {
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            dstFresh = false;
        } else if (CreateDirectoryW(ExtPath(dstDir).c_str(), nullptr)) {
            // created via the plain fallback: still fresh
        } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
            dstFresh = false;
        } else {
            ReportError(sink, dstDir, GetLastError());
            return; // nothing below can succeed
        }
    }

    std::vector<Item> run;
    auto flush = [&] {
        if (!run.empty()) queue.Push(std::move(run));
        run = std::vector<Item>();
    };

    WIN32_FIND_DATAW fd;
    // \\?\-prefixed: the copy I/O uses ExtPath, so the walk must too, or a
    // source subtree past MAX_PATH is silently skipped and a short copy is
    // reported as complete. Matches ScanTree in Robocopy.cpp exactly.
    HANDLE h = FindFirstFileExW(ExtPath(srcDir + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (Cancelled(sink)) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                IsExcludedDir(fd.cFileName))
                continue; // /XD: excluded folder — must match ScanTree exactly

            std::wstring src = srcDir + L"\\" + fd.cFileName;
            std::wstring dst = dstDir + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                flush(); // keep chunks single-directory
                WalkStream(src, dst, policy, sink, queue, bigs, dirsPost,
                           depth + 1);
            } else {
                unsigned long long size =
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                Item it{std::move(src), std::move(dst), size};
                if (size >= kBigFileBytes) {
                    // Big files are rare: classify inline (they must be routed
                    // to the ring here, a worker can't do that).
                    FileClass fc = dstFresh
                                       ? FileClass::Lonely
                                       : ClassifyOrCarried(it.src, it.dst, size,
                                                           fd.ftLastWriteTime);
                    if (PolicySkips(policy, fc)) {
                        if (sink.onSkip) sink.onSkip(it.src, it.size);
                    } else {
                        bigs.push_back(std::move(it));
                    }
                } else {
                    // Freshly created dir -> Lonely by construction, no dest
                    // stat anywhere. Otherwise classification is DEFERRED to
                    // the pool worker: the dest stat is one round-trip per
                    // file, and on a skip-heavy re-mirror those stats ARE the
                    // runtime — serial on the walk thread they idled all 16
                    // workers (measured: the whole transfer ran at one
                    // thread's stat rate).
                    if (!dstFresh) {
                        it.mtime = fd.ftLastWriteTime;
                        it.classify = true;
                    }
                    run.push_back(std::move(it));
                    if (run.size() >= kChunkFiles) flush();
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    flush();
    dirsPost.push_back(srcDir);
}

// ---- per-file copy (CopyFileEx) -------------------------------------------

struct ProgressCtx {
    const CopySink* sink;
    unsigned long long reported = 0; // survives retries: never double-count
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER /*total*/, LARGE_INTEGER transferred,
                            LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                            HANDLE, HANDLE, LPVOID param) {
    ProgressCtx* c = (ProgressCtx*)param;
    unsigned long long t = (unsigned long long)transferred.QuadPart;
    if (t > c->reported) {
        if (c->sink->onBytes) c->sink->onBytes(t - c->reported);
        c->reported = t;
    }
    return Cancelled(*c->sink) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// One file via CopyFileEx: kernel fast-path locally, server-side copy on SMB;
// copies data + attributes + last-write time (the old /COPY:DAT). Retries a
// locked file kRetries times; clears a read-only destination once (robocopy
// overwrites those too). Returns false on failure or cancel.
bool CopyOneFile(const Item& it, const CopySink& sink) {
    ProgressCtx ctx{&sink};
    bool clearedRo = false;
    // Prefix once: 16 workers rebuilding these on every retry iteration was
    // pure allocator churn on the hottest path.
    const std::wstring xs = ExtPath(it.src), xd = ExtPath(it.dst);
    for (int attempt = 0;; ++attempt) {
        BOOL cancelFlag = FALSE;
        if (CopyFileExW(xs.c_str(), xd.c_str(), CopyProgress, &ctx, &cancelFlag,
                        0)) {
            if (ctx.reported < it.size && sink.onBytes)
                sink.onBytes(it.size - ctx.reported); // tiny files see no callback delta
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_REQUEST_ABORTED) return false; // cancel: partial dst removed
        if (err == ERROR_ACCESS_DENIED && !clearedRo) {
            DWORD a = GetFileAttributesW(xd.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(xd.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                clearedRo = true;
                continue; // not a counted attempt
            }
        }
        if (attempt >= kRetries || Cancelled(sink)) {
            ReportError(sink, it.src, err);
            return false;
        }
        Sleep(kRetryWaitMs);
    }
}

bool DeleteSourceFile(const std::wstring& src) {
    const std::wstring xs = ExtPath(src);
    if (DeleteFileW(xs.c_str())) return true;
    DWORD a = GetFileAttributesW(xs.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(xs.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        return DeleteFileW(xs.c_str()) != 0;
    }
    return false;
}

// ---- big files: unbuffered overlapped ring --------------------------------

// qd buffers cycle read -> write-at-same-offset -> next read over an IOCP.
// Measured 3200 MB/s vs robocopy's 2430 on one cold 8 GiB file. Unbuffered on
// both sides: an 8 GiB stream through the page cache evicts everything else
// and the flush still gates completion. Returns 0, or a Win32 error
// (ERROR_REQUEST_ABORTED for cancel); partial destinations are deleted.
DWORD RingCopyFile(const Item& it, const CopySink& sink) {
    HANDLE hs = CreateFileW(ExtPath(it.src).c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (hs == INVALID_HANDLE_VALUE) return GetLastError();

    LARGE_INTEGER sz{};
    GetFileSizeEx(hs, &sz);
    const unsigned long long fileSize = (unsigned long long)sz.QuadPart;
    const unsigned long long alignedEnd = (fileSize + 4095) & ~4095ull;
    FILETIME tCreate{}, tAccess{}, tWrite{};
    GetFileTime(hs, &tCreate, &tAccess, &tWrite);

    HANDLE hd = CreateFileW(ExtPath(it.dst).c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (hd == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        CloseHandle(hs);
        return e;
    }

    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = (LONGLONG)alignedEnd; // preallocate
    SetFileInformationByHandle(hd, FileEndOfFileInfo, &eof, sizeof(eof));

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    DWORD fail = 0;
    if (!iocp || !CreateIoCompletionPort(hs, iocp, 1, 0) ||
        !CreateIoCompletionPort(hd, iocp, 2, 0))
        fail = GetLastError();

    struct Op {
        OVERLAPPED ov;
        unsigned char* buf;
        unsigned long long off;
        DWORD len;
    };
    // Ring buffers are reused across files: big files run strictly serially
    // (ExecutePlan / the bigs loop), and a batch of just-over-32-MiB files
    // paid an 8 x 8 MiB VirtualAlloc + zeroing per file for nothing. The
    // buffers live until process exit (the runner is short-lived). Geometry
    // (QD8 x 8 MiB) unchanged — this is allocation strategy, not tuning.
    static unsigned char* ringBufs[kRingDepth]{};
    Op ops[kRingDepth]{};
    for (int i = 0; i < kRingDepth; ++i) {
        if (!ringBufs[i])
            ringBufs[i] = (unsigned char*)VirtualAlloc(nullptr, kRingChunk,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_READWRITE);
        ops[i].buf = ringBufs[i];
        if (!ops[i].buf) fail = ERROR_NOT_ENOUGH_MEMORY;
    }

    unsigned long long nextOff = 0;
    int inFlight = 0;
    bool aborted = false;

    auto issueRead = [&](Op& o) -> bool {
        if (fail || aborted || nextOff >= alignedEnd) return false;
        o.off = nextOff;
        o.len = (DWORD)std::min<unsigned long long>(kRingChunk, alignedEnd - nextOff);
        nextOff += o.len;
        memset(&o.ov, 0, sizeof(o.ov));
        o.ov.Offset = (DWORD)(o.off & 0xFFFFFFFFull);
        o.ov.OffsetHigh = (DWORD)(o.off >> 32);
        if (!ReadFile(hs, o.buf, o.len, nullptr, &o.ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            fail = GetLastError();
            return false;
        }
        return true;
    };

    if (!fail)
        for (auto& o : ops)
            if (issueRead(o)) ++inFlight;

    while (inFlight > 0) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* pov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &pov, INFINITE);
        if (!pov) { fail = GetLastError(); break; } // port failure: bail out
        Op& o = *(Op*)pov; // OVERLAPPED is the first member

        if (!aborted && Cancelled(sink)) {
            aborted = true;
            CancelIoEx(hs, nullptr);
            CancelIoEx(hd, nullptr);
        }
        if (!ok) { // this op failed (or was cancelled): stop issuing new work
            if (!aborted && !fail) fail = GetLastError();
            --inFlight;
            continue;
        }
        if (aborted || fail) { --inFlight; continue; }

        if (key == 1) { // read done -> write the same offset, sector-rounded
            DWORD wlen = (bytes + 4095) & ~4095u;
            memset(&o.ov, 0, sizeof(o.ov));
            o.ov.Offset = (DWORD)(o.off & 0xFFFFFFFFull);
            o.ov.OffsetHigh = (DWORD)(o.off >> 32);
            if (!WriteFile(hd, o.buf, wlen, nullptr, &o.ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                fail = GetLastError();
                --inFlight;
            }
        } else {        // write done -> report real bytes, reuse the buffer
            unsigned long long real =
                std::min<unsigned long long>(bytes, fileSize - o.off);
            if (sink.onBytes && real) sink.onBytes(real);
            if (!issueRead(o)) --inFlight;
        }
    }

    CloseHandle(iocp);
    CloseHandle(hs);
    CloseHandle(hd);
    // ringBufs stay allocated for the next big file (see above).

    const std::wstring xd = ExtPath(it.dst);
    if (aborted || fail) {
        DeleteFileW(xd.c_str()); // no partial leftovers
        return aborted ? ERROR_REQUEST_ABORTED : fail;
    }

    // Trim the sector-rounded tail, stamp times (/COPY:T) and attributes
    // (/COPY:A) so the result matches what CopyFileEx would have produced.
    HANDLE ht = CreateFileW(xd.c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (ht == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER p;
    p.QuadPart = (LONGLONG)fileSize;
    SetFilePointerEx(ht, p, nullptr, FILE_BEGIN);
    SetEndOfFile(ht);
    SetFileTime(ht, &tCreate, &tAccess, &tWrite);
    CloseHandle(ht);
    DWORD a = GetFileAttributesW(ExtPath(it.src).c_str());
    if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(xd.c_str(), a);
    return 0;
}

// The ring is a LOCAL-disk tool. Measured on \\mp-fileserver (July 2026,
// 2 GiB): share→share via CopyFileExW ~0.2 s — SMB server-side copy offload,
// the data never crosses the wire — vs the ring's 6–7 s (every byte over the
// wire twice, ~30x slower). share→local: unbuffered ring reads bypass the
// redirector's read-ahead, 4.9–5.1 s vs 2.0–2.7 s buffered (~2x slower).
// local→share the ring won by ~6% — not worth a third code path. Rule: the
// ring runs only when BOTH ends are local; any remote end goes to CopyFileEx.
bool IsRemotePath(const std::wstring& p) {
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true; // UNC
    if (p.size() >= 2 && p[1] == L':') {
        wchar_t letter = (wchar_t)towupper(p[0]);
        if (letter < L'A' || letter > L'Z') return false;
        // Per-letter cache queried from every copy-pool thread; atomic (relaxed)
        // because the writes are idempotent but a plain int would be a data race.
        static std::atomic<int> cache[26]; // 0 unknown, 1 local, 2 remote
        std::atomic<int>& slot = cache[letter - L'A'];
        int v = slot.load(std::memory_order_relaxed);
        if (v == 0) {
            wchar_t root[4] = {letter, L':', L'\\', 0};
            v = (GetDriveTypeW(root) == DRIVE_REMOTE) ? 2 : 1;
            slot.store(v, std::memory_order_relaxed);
        }
        return v == 2;
    }
    return false;
}

// Big file with ring, falling back to CopyFileEx when the unbuffered open is
// refused (some filters/shares dislike FILE_FLAG_NO_BUFFERING).
bool CopyOneBig(const Item& it, const CopySink& sink) {
    if (IsRemotePath(it.src) || IsRemotePath(it.dst))
        return CopyOneFile(it, sink); // see IsRemotePath: measured, not a guess
    DWORD e = RingCopyFile(it, sink);
    if (e == 0) return true;
    if (e == ERROR_REQUEST_ABORTED) return false;
    return CopyOneFile(it, sink);
}

// ---- move -----------------------------------------------------------------

// Same-volume: a rename, ~free. Cross-volume: ERROR_NOT_SAME_DEVICE — copy
// then delete the source. The filesystem answers; no path heuristics.
bool MoveOneFile(const Item& it, const CopySink& sink) {
    const std::wstring xs = ExtPath(it.src), xd = ExtPath(it.dst);
    for (int pass = 0; pass < 2; ++pass) {
        if (MoveFileExW(xs.c_str(), xd.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (sink.onBytes && it.size) sink.onBytes(it.size);
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_NOT_SAME_DEVICE) break;
        if (err == ERROR_ACCESS_DENIED && pass == 0) {
            // read-only destination blocks the replace; clear it once
            DWORD a = GetFileAttributesW(xd.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(xd.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                continue;
            }
        }
        break; // odd rename failure: the copy path gets its chance below
    }
    bool ok = (it.size >= kBigFileBytes) ? CopyOneBig(it, sink)
                                         : CopyOneFile(it, sink);
    if (!ok) return false;
    if (!DeleteSourceFile(it.src)) {
        ReportError(sink, it.src, GetLastError());
        return false; // moved data but the source remains: surface it
    }
    return true;
}

// Whole-tree move fast path: destination absent and the rename sticks — the
// entire tree moves in one metadata operation. Afterwards the moved tree is
// walked (cheap) so the progress totals still add up.
bool TryRenameTree(const RoboJob& job, const CopySink& sink) {
    // Same guard as TryQuickRenameMove: a whole-tree rename cannot honor
    // active directory exclusions — it would move the excluded caches along.
    if (AnyExcludedDirs()) return false;
    if (GetFileAttributesW(ExtPath(job.dstDir).c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    if (!MoveFileExW(ExtPath(job.srcDir).c_str(), ExtPath(job.dstDir).c_str(), 0))
        return false;

    struct Reporter {
        const CopySink& sink;
        void Walk(const std::wstring& dir, int depth = 0) {
            if (depth >= kMaxWalkDepth) return; // totals only; safe to stop
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(ExtPath(dir + L"\\*").c_str(),
                                        FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                std::wstring p = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    Walk(p, depth + 1);
                } else {
                    unsigned long long size =
                        ((unsigned long long)fd.nFileSizeHigh << 32) |
                        fd.nFileSizeLow;
                    if (sink.onBytes && size) sink.onBytes(size);
                    if (sink.onFileDone) sink.onFileDone(p, size);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } rep{sink};
    rep.Walk(job.dstDir);
    return true;
}

// ---- plan execution -------------------------------------------------------

// One work item, routed to the right primitive. Returns false on failure or
// cancel (the caller distinguishes via Cancelled()).
bool ProcessItem(const Item& it, Operation op, const CopySink& sink) {
    if (sink.onFileStart) sink.onFileStart(it.src);
    bool ok;
    if (op == Operation::Move)
        ok = MoveOneFile(it, sink);
    else if (it.size >= kBigFileBytes)
        ok = CopyOneBig(it, sink);
    else
        ok = CopyOneFile(it, sink);
    if (ok && sink.onFileDone) sink.onFileDone(it.src, it.size);
    return ok;
}

// Small files: thread pool over directory-sharded chunks. One thread works one
// directory's chunk at a time (NTFS serializes creates per directory), split
// at kChunkFiles so a single flat directory still occupies every thread.
void RunSmallPool(const std::vector<Item>& items, Operation op,
                  const CopySink& sink, std::atomic<bool>& anyError,
                  std::atomic<bool>& anyCopied) {
    if (items.empty()) return;

    std::vector<std::pair<size_t, size_t>> chunks; // [begin, end)
    {
        auto dirOf = [](const std::wstring& p) {
            size_t cut = p.find_last_of(L'\\');
            return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
        };
        size_t start = 0;
        std::wstring cur = dirOf(items[0].src);
        for (size_t i = 1; i <= items.size(); ++i) {
            bool boundary = (i == items.size()) || (i - start >= kChunkFiles);
            if (!boundary) {
                std::wstring d = dirOf(items[i].src);
                boundary = (d != cur);
                if (boundary) cur = std::move(d);
            }
            if (boundary) {
                chunks.emplace_back(start, i);
                start = i;
            }
        }
    }

    const int threads =
        (int)std::min<size_t>((size_t)PoolThreads(), chunks.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (;;) {
                size_t c = next.fetch_add(1);
                if (c >= chunks.size()) return;
                for (size_t i = chunks[c].first; i < chunks[c].second; ++i) {
                    if (Cancelled(sink)) return;
                    if (ProcessItem(items[i], op, sink)) anyCopied = true;
                    else if (!Cancelled(sink)) anyError = true;
                }
            }
        });
    for (auto& t : pool) t.join();
}

void ExecutePlan(const Plan& plan, Operation op, const CopySink& sink,
                 std::atomic<bool>& anyError, std::atomic<bool>& anyCopied) {
    // Skips first: they advance the green bar instantly — the scan already
    // proved nothing needs doing for them. (Robocopy interleaved them; the
    // order carries no information, /MT never had one.)
    for (const Item& it : plan.skips) {
        if (Cancelled(sink)) return;
        if (sink.onSkip) sink.onSkip(it.src, it.size);
    }

    RunSmallPool(plan.smallItems, op, sink, anyError, anyCopied);

    for (const Item& it : plan.big) {
        if (Cancelled(sink)) return;
        if (ProcessItem(it, op, sink)) anyCopied = true;
        else if (!Cancelled(sink)) anyError = true;
    }
}

} // namespace

int RunNativeJobs(Operation op, const std::vector<RoboJob>& jobs,
                  Conflict policy, const CopySink& sink) {
    std::atomic<bool> anyError{false}, anyCopied{false};

    for (const RoboJob& job : jobs) {
        if (Cancelled(sink)) break;

        if (job.files.empty()) {
            // The destination's parents may not exist yet (robocopy created
            // the full path; so must we). Only the parents — TryRenameTree
            // needs dstDir itself absent for the rename fast path.
            CreateDirDeep(ParentOf(job.dstDir));
            // Whole-tree job. A move whose destination doesn't exist yet may
            // collapse into a single rename (same volume only — the
            // filesystem decides).
            if (op == Operation::Move && TryRenameTree(job, sink)) {
                anyCopied = true;
                continue;
            }

            // Streaming: the pool starts copying the first directory while
            // the walk is still classifying the rest of the tree. Big files
            // run after the pool (the ring wants the bandwidth to itself).
            ChunkQueue queue;
            std::vector<Item> bigs;
            std::vector<std::wstring> dirsPost;
            std::vector<std::thread> pool;
            const int threads = PoolThreads();
            for (int t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    std::vector<Item> chunk;
                    while (queue.Pop(chunk))
                        for (const Item& it : chunk) {
                            if (Cancelled(sink)) return;
                            if (it.classify) {
                                // Deferred dest stat (see WalkStream): decide
                                // skip-vs-copy here so 16 workers share the
                                // stat cost instead of the walk thread alone.
                                // Carried scan verdicts skip the stat entirely.
                                FileClass fc = ClassifyOrCarried(
                                    it.src, it.dst, it.size, it.mtime);
                                if (PolicySkips(policy, fc)) {
                                    if (sink.onSkip) sink.onSkip(it.src, it.size);
                                    continue;
                                }
                            }
                            if (ProcessItem(it, op, sink)) anyCopied = true;
                            else if (!Cancelled(sink)) anyError = true;
                        }
                });
            WalkStream(job.srcDir, job.dstDir, policy, sink, queue, bigs,
                       dirsPost);
            queue.Close();
            for (auto& t : pool) t.join();

            for (const Item& it : bigs) {
                if (Cancelled(sink)) break;
                if (ProcessItem(it, op, sink)) anyCopied = true;
                else if (!Cancelled(sink)) anyError = true;
            }

            // Move cleanup: drop emptied source dirs, deepest first. Fails
            // harmlessly wherever skipped/failed files remain — robocopy
            // leaves those directories behind too.
            if (op == Operation::Move && !Cancelled(sink))
                for (const auto& d : dirsPost)
                    RemoveDirectoryW(ExtPath(d).c_str());
        } else {
            // Loose files into dstDir (which may not exist yet).
            CreateDirDeep(job.dstDir);
            Plan plan;
            for (size_t i = 0; i < job.files.size(); ++i) {
                const std::wstring& f = job.files[i];
                std::wstring src = job.srcDir + L"\\" + f;
                // dstNames[i] renames a same-folder copy ("x - Kopie"); absent
                // => keep the source name.
                std::wstring dst = job.dstDir + L"\\" +
                    (i < job.dstNames.size() ? job.dstNames[i] : f);
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                // ExtPath is required here: ScanJobs stats the same file via
                // Ext(), so a bare query would make the engine drop a
                // >MAX_PATH loose file the scan already counted — a silent
                // short copy reported as complete.
                if (!GetFileAttributesExW(ExtPath(src).c_str(),
                                          GetFileExInfoStandard, &fa))
                    continue; // vanished since planning; scan skipped it too
                unsigned long long size =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                FileClass fc =
                    ClassifyOrCarried(src, dst, size, fa.ftLastWriteTime);
                Item it{std::move(src), std::move(dst), size};
                if (PolicySkips(policy, fc))    plan.skips.push_back(std::move(it));
                else if (size >= kBigFileBytes) plan.big.push_back(std::move(it));
                else                            plan.smallItems.push_back(std::move(it));
            }
            ExecutePlan(plan, op, sink, anyError, anyCopied);
        }
    }
    return anyError ? 8 : (anyCopied ? 1 : 0);
}

bool TryQuickRenameMove(const RoboJob& job) {
    if (!job.files.empty()) return false; // whole-tree jobs only
    // Exclusions active (Unreal preset): a whole-tree rename would move the
    // excluded cache folders along — silently ignoring the choice the user
    // just made in the prompt. Fall through to the per-file path, which is
    // the only way to honor them.
    if (AnyExcludedDirs()) return false;
    if (GetFileAttributesW(ExtPath(job.dstDir).c_str()) != INVALID_FILE_ATTRIBUTES)
        return false; // destination exists: conflicts possible, scan first
    CreateDirDeep(ParentOf(job.dstDir));
    return MoveFileExW(ExtPath(job.srcDir).c_str(), ExtPath(job.dstDir).c_str(),
                       0) != 0;
}

bool UseNativeEngine() {
    wchar_t buf[32];
    DWORD n = GetEnvironmentVariableW(L"ANGELCOPY_ENGINE", buf, 32);
    return !(n && n < 32 && _wcsicmp(buf, L"robocopy") == 0);
}

int RunJobsConsole(Operation op, const std::vector<RoboJob>& jobs,
                   Conflict policy) {
    if (!UseNativeEngine()) return RunJobs(op, jobs, policy);

    std::mutex m;
    CopySink sink;
    sink.onError = [&m](const std::wstring& msg) {
        std::lock_guard<std::mutex> lock(m);
        fwprintf(stderr, L"[AngelCOPY] %s\n", msg.c_str());
    };
    for (const RoboJob& job : jobs)
        wprintf(L"\n[AngelCOPY] %s  ->  %s\n", job.srcDir.c_str(),
                job.dstDir.c_str());
    return RunNativeJobs(op, jobs, policy, sink);
}

} // namespace angelcopy
