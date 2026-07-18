#include "NativeCopy.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace angelcopy {

namespace {

// Measured tuning (bench/bench.cpp). Do not change without re-measuring.
constexpr unsigned long long kBigFileBytes = 32ull << 20; // ring threshold
constexpr DWORD  kRingChunk = 8u << 20;                   // 8 MiB
constexpr int    kRingDepth = 8;                          // QD8
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
// the shell; UNC needs the \\?\UNC\ form.
std::wstring ExtPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

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

std::wstring WinErrText(DWORD err) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out = (n && msg) ? std::wstring(msg, n) : L"";
    if (msg) LocalFree(msg);
    while (!out.empty() &&
           (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
        out.pop_back();
    return out;
}

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

// Which classes the policy leaves uncopied (Same is never copied).
bool PolicySkips(Conflict policy, FileClass fc) {
    switch (fc) {
    case FileClass::Same:      return true;
    case FileClass::Lonely:    return false;
    case FileClass::DiffNewer: return policy == Conflict::Skip;
    case FileClass::DiffOlder: return policy != Conflict::Replace;
    }
    return false;
}

struct Item {
    std::wstring src, dst;
    unsigned long long size = 0;
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

// Bounded-by-nothing chunk queue between the walker and the copy pool. The
// walk streams: the pool is already copying while later directories are still
// being enumerated. (The first version walked the WHOLE tree before the first
// byte moved — on a 500k-file folder the dialog sat at 0% for the entire
// walk. Robocopy never had that pause; neither may we.)
struct ChunkQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<Item>> q;
    bool closed = false;

    void Push(std::vector<Item>&& c) {
        {
            std::lock_guard<std::mutex> l(m);
            q.push_back(std::move(c));
        }
        cv.notify_one();
    }
    bool Pop(std::vector<Item>& out) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return closed || !q.empty(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    void Close() {
        {
            std::lock_guard<std::mutex> l(m);
            closed = true;
        }
        cv.notify_all();
    }
};

// Streaming walk mirroring ScanTree exactly (reparse points skipped — the old
// /XJ — so outcomes match the scan's totals). Creates destination dirs on the
// way down (CreateDirectoryEx copies the source dir's attributes), reports
// skips immediately (the green bar advances during the walk), collects big
// files for the ring, and enqueues small files in per-directory chunks. A
// chunk is flushed before recursing into a subdirectory and at 256 files, so
// copying starts as soon as the first directory is read.
void WalkStream(const std::wstring& srcDir, const std::wstring& dstDir,
                Conflict policy, const CopySink& sink, ChunkQueue& queue,
                std::vector<Item>& bigs, std::vector<std::wstring>& dirsPost) {
    if (Cancelled(sink)) return;
    if (!CreateDirectoryExW(ExtPath(srcDir).c_str(), ExtPath(dstDir).c_str(),
                            nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        if (!CreateDirectoryW(ExtPath(dstDir).c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
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
    HANDLE h = FindFirstFileExW((srcDir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (Cancelled(sink)) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            std::wstring src = srcDir + L"\\" + fd.cFileName;
            std::wstring dst = dstDir + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                flush(); // keep chunks single-directory
                WalkStream(src, dst, policy, sink, queue, bigs, dirsPost);
            } else {
                unsigned long long size =
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                unsigned long long dstSize = 0;
                FileClass fc = ClassifyFile(dst, size, fd.ftLastWriteTime, dstSize);
                Item it{std::move(src), std::move(dst), size};
                if (PolicySkips(policy, fc)) {
                    if (sink.onSkip) sink.onSkip(it.src, it.size);
                } else if (size >= kBigFileBytes) {
                    bigs.push_back(std::move(it));
                } else {
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
    for (int attempt = 0;; ++attempt) {
        BOOL cancelFlag = FALSE;
        if (CopyFileExW(ExtPath(it.src).c_str(), ExtPath(it.dst).c_str(),
                        CopyProgress, &ctx, &cancelFlag, 0)) {
            if (ctx.reported < it.size && sink.onBytes)
                sink.onBytes(it.size - ctx.reported); // tiny files see no callback delta
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_REQUEST_ABORTED) return false; // cancel: partial dst removed
        if (err == ERROR_ACCESS_DENIED && !clearedRo) {
            DWORD a = GetFileAttributesW(ExtPath(it.dst).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(ExtPath(it.dst).c_str(),
                                   a & ~FILE_ATTRIBUTE_READONLY);
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
    if (DeleteFileW(ExtPath(src).c_str())) return true;
    DWORD a = GetFileAttributesW(ExtPath(src).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(ExtPath(src).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        return DeleteFileW(ExtPath(src).c_str()) != 0;
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
    Op ops[kRingDepth]{};
    for (auto& o : ops) {
        o.buf = (unsigned char*)VirtualAlloc(nullptr, kRingChunk,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
        if (!o.buf) fail = ERROR_NOT_ENOUGH_MEMORY;
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
    for (auto& o : ops)
        if (o.buf) VirtualFree(o.buf, 0, MEM_RELEASE);

    if (aborted || fail) {
        DeleteFileW(ExtPath(it.dst).c_str()); // no partial leftovers
        return aborted ? ERROR_REQUEST_ABORTED : fail;
    }

    // Trim the sector-rounded tail, stamp times (/COPY:T) and attributes
    // (/COPY:A) so the result matches what CopyFileEx would have produced.
    HANDLE ht = CreateFileW(ExtPath(it.dst).c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (ht == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER p;
    p.QuadPart = (LONGLONG)fileSize;
    SetFilePointerEx(ht, p, nullptr, FILE_BEGIN);
    SetEndOfFile(ht);
    SetFileTime(ht, &tCreate, &tAccess, &tWrite);
    CloseHandle(ht);
    DWORD a = GetFileAttributesW(ExtPath(it.src).c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(ExtPath(it.dst).c_str(), a);
    return 0;
}

// Big file with ring, falling back to CopyFileEx when the unbuffered open is
// refused (some filters/shares dislike FILE_FLAG_NO_BUFFERING).
bool CopyOneBig(const Item& it, const CopySink& sink) {
    DWORD e = RingCopyFile(it, sink);
    if (e == 0) return true;
    if (e == ERROR_REQUEST_ABORTED) return false;
    return CopyOneFile(it, sink);
}

// ---- move -----------------------------------------------------------------

// Same-volume: a rename, ~free. Cross-volume: ERROR_NOT_SAME_DEVICE — copy
// then delete the source. The filesystem answers; no path heuristics.
bool MoveOneFile(const Item& it, const CopySink& sink) {
    for (int pass = 0; pass < 2; ++pass) {
        if (MoveFileExW(ExtPath(it.src).c_str(), ExtPath(it.dst).c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            if (sink.onBytes && it.size) sink.onBytes(it.size);
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_NOT_SAME_DEVICE) break;
        if (err == ERROR_ACCESS_DENIED && pass == 0) {
            // read-only destination blocks the replace; clear it once
            DWORD a = GetFileAttributesW(ExtPath(it.dst).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(ExtPath(it.dst).c_str(),
                                   a & ~FILE_ATTRIBUTE_READONLY);
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
    if (GetFileAttributesW(job.dstDir.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    if (!MoveFileExW(ExtPath(job.srcDir).c_str(), ExtPath(job.dstDir).c_str(), 0))
        return false;

    struct Reporter {
        const CopySink& sink;
        void Walk(const std::wstring& dir) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic,
                                        &fd, FindExSearchNameMatch, nullptr, 0);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                std::wstring p = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    Walk(p);
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
                if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa))
                    continue; // vanished since planning; scan skipped it too
                unsigned long long size =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                unsigned long long dstSize = 0;
                FileClass fc = ClassifyFile(dst, size, fa.ftLastWriteTime, dstSize);
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
    if (GetFileAttributesW(job.dstDir.c_str()) != INVALID_FILE_ATTRIBUTES)
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
