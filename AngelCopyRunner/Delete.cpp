#include "Delete.h"

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace angelcopy {

namespace {

std::wstring StripSep(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// \\?\ form so paths longer than MAX_PATH work. Explorer refuses those; we
// should not inherit that limit when deleting.
std::wstring Ext(const std::wstring& p) {
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + p.substr(2);   // \\server\share -> \\?\UNC\server\share
    return L"\\\\?\\" + p;
}

std::wstring BaseName(const std::wstring& p) {
    std::wstring s = StripSep(p);
    size_t i = s.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? s : s.substr(i + 1);
}

std::wstring SysError(DWORD e) {
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = (n && buf) ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

void ReportError(const DeleteSink& sink, const std::wstring& path, DWORD e) {
    if (sink.onError)
        sink.onError(path + L"  \x2014  " + SysError(e));
}

bool Cancelled(const DeleteSink& sink) {
    return sink.cancelled && sink.cancelled();
}

bool DeleteOneFile(const std::wstring& path, unsigned long long size,
                   const DeleteSink& sink) {
    std::wstring x = Ext(path);
    if (DeleteFileW(x.c_str())) {
        if (sink.onFile) sink.onFile(path, size);
        return true;
    }
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
        // Most likely read-only; Explorer prompts here, we just clear it.
        SetFileAttributesW(x.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(x.c_str())) {
            if (sink.onFile) sink.onFile(path, size);
            return true;
        }
        e = GetLastError();
    }
    ReportError(sink, path, e);
    return false;
}

bool RemoveDir(const std::wstring& dir, const DeleteSink& sink) {
    std::wstring x = Ext(dir);
    if (RemoveDirectoryW(x.c_str())) return true;
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(x.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(x.c_str())) return true;
        e = GetLastError();
    }
    ReportError(sink, dir, e);
    return false;
}

// ---- parallel tree delete --------------------------------------------------
// Measured (bench scenario D, 10k x 64 KiB): sequential 1358-1656 ms vs
// dir-sharded 8-thread pool 330-380 ms — 3.9x. (An earlier note claimed
// parallel deletion buys ~20% at best; that measured NAIVE parallelism. One
// directory's files per chunk sidesteps the NTFS per-directory serialization,
// exactly like the copy pool.) 16 threads measured no better than 8.

constexpr int    kDelThreads = 8;
constexpr size_t kDelChunkFiles = 256;

// Cap recursion so a pathologically deep tree (\\?\ paths allow ~32k chars,
// i.e. thousands of nesting levels) can't overflow the ~1 MB stack and crash
// the process mid-delete. At the cap we stop descending and report it as an
// error rather than crash. No normal tree comes near this.
constexpr int    kMaxDepth = 900;

struct DelItem {
    std::wstring path;
    unsigned long long size;
};

struct DelQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<DelItem>> q;
    bool closed = false;

    void Push(std::vector<DelItem>&& c) {
        {
            std::lock_guard<std::mutex> l(m);
            q.push_back(std::move(c));
        }
        cv.notify_one();
    }
    bool Pop(std::vector<DelItem>& out) {
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

// Streaming walk: enqueues one directory's files per chunk while the pool is
// already deleting earlier directories. Junctions/symlinks are removed as
// links inline, never followed. Real directories are collected post-order for
// the sequential bottom-up removal afterwards.
void WalkDelete(const std::wstring& dir, const DeleteSink& sink, DelQueue& q,
                std::vector<std::wstring>& dirsPost, std::atomic<bool>& ok,
                int depth = 0) {
    if (Cancelled(sink)) return;
    if (depth >= kMaxDepth) {
        ReportError(sink, dir, ERROR_STACK_OVERFLOW);
        ok = false;
        return;
    }

    std::vector<DelItem> run;
    auto flush = [&] {
        if (!run.empty()) q.Push(std::move(run));
        run = std::vector<DelItem>();
    };

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((Ext(dir) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) { ReportError(sink, dir, e); ok = false; }
    } else {
        do {
            if (Cancelled(sink)) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring child = dir + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    // Junction/symlink: remove the link, never follow it —
                    // following would delete the target's contents.
                    if (!RemoveDir(child, sink)) ok = false;
                } else {
                    flush(); // keep chunks single-directory
                    WalkDelete(child, sink, q, dirsPost, ok, depth + 1);
                }
            } else {
                unsigned long long size =
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                run.push_back(DelItem{std::move(child), size});
                if (run.size() >= kDelChunkFiles) flush();
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    flush();
    dirsPost.push_back(dir);
}

// Returns false if anything under `dir` could not be removed.
bool DeleteTree(const std::wstring& dir, const DeleteSink& sink) {
    DelQueue queue;
    std::vector<std::wstring> dirsPost;
    std::atomic<bool> ok{true};

    std::vector<std::thread> pool;
    for (int t = 0; t < kDelThreads; ++t)
        pool.emplace_back([&] {
            std::vector<DelItem> chunk;
            while (queue.Pop(chunk))
                for (const DelItem& it : chunk) {
                    if (Cancelled(sink)) return;
                    if (!DeleteOneFile(it.path, it.size, sink)) ok = false;
                }
        });

    WalkDelete(dir, sink, queue, dirsPost, ok);
    queue.Close();
    for (auto& t : pool) t.join();

    if (Cancelled(sink)) return false;
    // Bottom-up: every dir was pushed after its children, so plain order is
    // already deepest-first. Only report removal errors when the files went —
    // a dir left non-empty by a failed file delete is that failure, not a new
    // one.
    for (const auto& d : dirsPost) {
        if (Cancelled(sink)) return false;
        if (ok.load()) {
            if (!RemoveDir(d, sink)) ok = false;
        } else {
            RemoveDirectoryW(Ext(d).c_str());
        }
    }
    return ok.load();
}

// The pre-delete count is pure enumeration (sizes come free with the find
// data), so its cost is one FindFirstFile round-trip per directory —
// latency-bound on network/USB targets exactly like deletion itself. The
// serial recursive walk counted one directory at a time; the confirmation on
// a big tree made the user wait for minutes. Same cure as the deleter: a
// directory-granular work queue over kDelThreads workers, each accumulating
// into its own DeleteScan, merged at the end.
struct ScanWork {
    struct Entry {
        std::wstring dir;
        int depth = 0;
        unsigned long long cluster = 0; // ScanAllocated only; 0 for the count
    };
    std::mutex m;
    std::condition_variable cv;
    std::deque<Entry> q;
    int active = 0;   // directories currently being enumerated
    bool done = false;

    void Push(std::wstring dir, int depth, unsigned long long cluster = 0) {
        {
            std::lock_guard<std::mutex> l(m);
            q.push_back(Entry{std::move(dir), depth, cluster});
        }
        cv.notify_one();
    }
};

// Shared worker loop: kDelThreads pop directories until the queue is drained
// and nobody is left who could refill it. `perDir(entry, workerIndex)` does
// the actual enumeration (and pushes subdirectories back).
template <typename PerDir>
void DrainScanWork(ScanWork& work, ScanProgress* prog, PerDir perDir) {
    std::vector<std::thread> pool;
    for (int t = 0; t < kDelThreads; ++t)
        pool.emplace_back([&, t] {
            for (;;) {
                std::unique_lock<std::mutex> l(work.m);
                work.cv.wait(l, [&] { return work.done || !work.q.empty(); });
                if (work.q.empty()) return; // done and drained
                ScanWork::Entry e = std::move(work.q.front());
                work.q.pop_front();
                ++work.active;
                l.unlock();
                // On cancel just drain the queue; no further enumeration.
                if (!(prog && prog->cancel.load(std::memory_order_relaxed)))
                    perDir(e, t);
                l.lock();
                --work.active;
                // Last one out: queue empty and nobody left who could refill it.
                if (work.q.empty() && work.active == 0) {
                    work.done = true;
                    work.cv.notify_all();
                }
            }
        });
    for (auto& th : pool) th.join();
}

// Enumerate ONE directory into acc; subdirectories go back on the queue.
void ScanOneDir(const std::wstring& dir, int depth, DeleteScan& acc,
                ScanWork& work, ScanProgress* prog) {
    if (depth >= kMaxDepth) return; // matches WalkDelete's cap; stop descending
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((Ext(dir) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            acc.dirs += 1;
            if (prog) prog->dirs.fetch_add(1, std::memory_order_relaxed);
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                work.Push(dir + L"\\" + fd.cFileName, depth + 1);
        } else {
            unsigned long long sz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            acc.files += 1;
            acc.bytes += sz;
            if (prog) {
                prog->files.fetch_add(1, std::memory_order_relaxed);
                prog->bytes.fetch_add(sz, std::memory_order_relaxed);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

DeleteScan ScanDelete(const std::vector<std::wstring>& targets,
                      ScanProgress* prog) {
    DeleteScan s;
    ScanWork work;
    for (const auto& raw : targets) {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        std::wstring t = StripSep(raw);
        if (t.empty()) continue;
        s.sample.push_back(BaseName(t));

        DWORD attr = GetFileAttributesW(Ext(t).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            s.dirs += 1;
            if (prog) prog->dirs.fetch_add(1, std::memory_order_relaxed);
            if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
                work.q.push_back(ScanWork::Entry{t, 0, 0}); // pre-pool: no lock yet
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(Ext(t).c_str(), GetFileExInfoStandard, &fa)) {
                unsigned long long sz =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                s.files += 1;
                s.bytes += sz;
                if (prog) {
                    prog->files.fetch_add(1, std::memory_order_relaxed);
                    prog->bytes.fetch_add(sz, std::memory_order_relaxed);
                }
            }
        }
    }
    if (work.q.empty()) return s;

    std::vector<DeleteScan> local(kDelThreads);
    DrainScanWork(work, prog, [&](const ScanWork::Entry& e, int t) {
        ScanOneDir(e.dir, e.depth, local[t], work, prog);
    });
    for (const auto& l : local) {
        s.files += l.files;
        s.dirs += l.dirs;
        s.bytes += l.bytes;
    }
    return s;
}

// ---- size on disk ----------------------------------------------------------
// What Explorer's "Size on disk" shows: the true allocation for compressed and
// sparse files (GetCompressedFileSize — the only case needing an extra
// round-trip), and the find-data size rounded UP to the volume's cluster for
// ordinary files. Runs on the same 8-worker directory queue as the count.

unsigned long long ClusterOf(const std::wstring& path) {
    std::wstring root;
    if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 2) + L"\\";
    } else if (path.rfind(L"\\\\", 0) == 0) {
        size_t s1 = path.find(L'\\', 2);
        size_t s2 = (s1 == std::wstring::npos) ? s1 : path.find(L'\\', s1 + 1);
        root = (s2 == std::wstring::npos) ? path + L"\\" : path.substr(0, s2 + 1);
    }
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!root.empty() &&
        GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &fc, &tc) && spc && bps)
        return (unsigned long long)spc * bps;
    return 4096; // can't tell (odd share): the common NTFS default
}

unsigned long long AllocOfFile(const std::wstring& path, DWORD attr,
                               unsigned long long fdSize,
                               unsigned long long cluster) {
    if (attr & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) {
        ULARGE_INTEGER v{};
        v.LowPart = GetCompressedFileSizeW(Ext(path).c_str(), &v.HighPart);
        if (v.LowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
            return 0; // vanished/unreadable: count nothing rather than lie
        return v.QuadPart; // already the true allocation
    }
    return ((fdSize + cluster - 1) / cluster) * cluster;
}

void AllocOneDir(const ScanWork::Entry& e, unsigned long long& acc,
                 ScanWork& work, ScanProgress* prog) {
    if (e.depth >= kMaxDepth) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((Ext(e.dir) + L"\\*").c_str(), FindExInfoBasic,
                                &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                work.Push(e.dir + L"\\" + fd.cFileName, e.depth + 1, e.cluster);
        } else {
            unsigned long long sz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            unsigned long long a = AllocOfFile(e.dir + L"\\" + fd.cFileName,
                                               fd.dwFileAttributes, sz, e.cluster);
            acc += a;
            if (prog) prog->bytes.fetch_add(a, std::memory_order_relaxed);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

unsigned long long ScanAllocated(const std::vector<std::wstring>& targets,
                                 ScanProgress* prog) {
    unsigned long long total = 0;
    ScanWork work;
    for (const auto& raw : targets) {
        if (prog && prog->cancel.load(std::memory_order_relaxed)) break;
        std::wstring t = StripSep(raw);
        if (t.empty()) continue;
        DWORD attr = GetFileAttributesW(Ext(t).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        unsigned long long cluster = ClusterOf(t);
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
                work.q.push_back(ScanWork::Entry{t, 0, cluster});
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(Ext(t).c_str(), GetFileExInfoStandard, &fa)) {
                unsigned long long sz =
                    ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                unsigned long long a =
                    AllocOfFile(t, fa.dwFileAttributes, sz, cluster);
                total += a;
                if (prog) prog->bytes.fetch_add(a, std::memory_order_relaxed);
            }
        }
    }
    if (work.q.empty()) return total;

    std::vector<unsigned long long> local(kDelThreads, 0);
    DrainScanWork(work, prog, [&](const ScanWork::Entry& e, int t) {
        AllocOneDir(e, local[t], work, prog);
    });
    for (unsigned long long l : local) total += l;
    return total;
}

int DeleteTargets(const std::vector<std::wstring>& targets, const DeleteSink& sink) {
    bool ok = true;
    for (const auto& raw : targets) {
        if (Cancelled(sink)) break;
        std::wstring t = StripSep(raw);
        if (t.empty()) continue;

        DWORD attr = GetFileAttributesW(Ext(t).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            ReportError(sink, t, GetLastError());
            ok = false;
            continue;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!RemoveDir(t, sink)) ok = false;
            } else if (!DeleteTree(t, sink)) {
                ok = false;
            }
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            unsigned long long size = 0;
            if (GetFileAttributesExW(Ext(t).c_str(), GetFileExInfoStandard, &fa))
                size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (!DeleteOneFile(t, size, sink)) ok = false;
        }
    }
    return ok ? 0 : 8;
}

} // namespace angelcopy
