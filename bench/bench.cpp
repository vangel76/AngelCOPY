// bench.cpp — AngelCOPY native-engine benchmark prototype.
//
// Question: can a native Win32 copy engine beat robocopy on THIS machine?
// Standalone tool, no dependency on the rest of the repo. Numbers decide
// whether a native engine replaces robocopy — measure first, build second.
//
//   bench.exe <workdir> [--quick] [--keep] [--small N] [--big GiB] [--move GiB]
//
// Scenarios:
//   A  many small files : robocopy /E /MT:64            vs 64-thread CopyFileW pool
//   B  one big file     : robocopy, robocopy /J, CopyFileW,
//                         CopyFileEx(COPY_FILE_NO_BUFFERING),
//                         overlapped unbuffered ring (two QD/chunk configs)
//   C  same-volume move : robocopy /MOV                 vs MoveFileExW (rename)
//
// Cold cache: every source is evicted before every timed run by opening it
// with FILE_FLAG_NO_BUFFERING — a non-cached open makes the cache manager
// purge that file's cached pages. (A warm-cache run measures nothing; the
// repo already learned that the hard way: 3165 "MB/s".)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>

using std::wstring;

// ------------------------------------------------------------------ utilities

static void die(const char* what) {
    fprintf(stderr, "FATAL: %s (GetLastError=%lu)\n", what, GetLastError());
    exit(1);
}

static double Now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t Rand64() {
    uint64_t x = g_rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    g_rng = x; return x;
}

static const DWORD GEN_CHUNK = 1u << 20;    // 1 MiB
static uint8_t* g_random = nullptr;

static void InitRandomBuf() {
    g_random = (uint8_t*)VirtualAlloc(nullptr, GEN_CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_random) die("VirtualAlloc random buffer");
    for (DWORD i = 0; i < GEN_CHUNK; i += 8) *(uint64_t*)(g_random + i) = Rand64();
}

// Random content defeats dedup/compression; the per-chunk counter patch keeps
// generation cheap without producing identical chunks.
static void WriteRandomFile(const wstring& path, uint64_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) die("create data file");
    uint64_t written = 0, counter = Rand64();
    while (written < size) {
        DWORD n = (DWORD)std::min<uint64_t>(GEN_CHUNK, size - written);
        *(uint64_t*)g_random = counter++;
        DWORD got = 0;
        if (!WriteFile(h, g_random, n, &got, nullptr) || got != n) die("write data file");
        written += n;
    }
    CloseHandle(h);
}

static uint64_t FileSizeOf(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return ~0ull;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

// Opening with FILE_FLAG_NO_BUFFERING purges the file's cached pages.
static void EvictFile(const wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void EnumTree(const wstring& dir, const wstring& rel,
                     std::vector<wstring>& dirs, std::vector<wstring>& files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wstring r = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs.push_back(r);
            EnumTree(dir + L"\\" + fd.cFileName, r, dirs, files);
        } else {
            files.push_back(r);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void EvictTree(const wstring& root) {
    std::vector<wstring> dirs, files;
    EnumTree(root, L"", dirs, files);
    for (auto& f : files) EvictFile(root + L"\\" + f);
}

static size_t CountTree(const wstring& root) {
    std::vector<wstring> dirs, files;
    EnumTree(root, L"", dirs, files);
    return files.size();
}

static void RemoveTree(const wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// ------------------------------------------------------------------ processes

static wstring RobocopyExe() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return wstring(sys) + L"\\robocopy.exe";
}

static double RunProcess(wstring cmdline, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = nul; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    double t0 = Now();
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) die("CreateProcess");
    WaitForSingleObject(pi.hProcess, INFINITE);
    double dt = Now() - t0;
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return dt;
}

static double RunRobocopy(const wstring& args, bool* ok) {
    DWORD code = 0;
    double dt = RunProcess(L"\"" + RobocopyExe() + L"\" " + args, &code);
    *ok = code < 8;
    return dt;
}

static const wchar_t* RQUIET = L"/R:0 /W:0 /NJH /NJS /NP /NFL /NDL";

// -------------------------------------------------------------- native engines

static bool NativeTreeCopy(const wstring& src, const wstring& dst, int threads) {
    std::vector<wstring> dirs, files;
    EnumTree(src, L"", dirs, files);
    CreateDirectoryW(dst.c_str(), nullptr);
    for (auto& d : dirs) CreateDirectoryW((dst + L"\\" + d).c_str(), nullptr);
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> ok{ true };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= files.size()) return;
            if (!CopyFileW((src + L"\\" + files[i]).c_str(),
                           (dst + L"\\" + files[i]).c_str(), FALSE)) ok = false;
        }
    });
    for (auto& t : pool) t.join();
    return ok;
}

// Same primitive (CopyFileW), different work distribution: each thread grabs a
// whole DIRECTORY at a time, so at most one thread creates files in any given
// directory. Tests the hypothesis that the interleaved pool collapses at high
// thread counts because file creation serializes on NTFS directory metadata.
static bool NativeTreeCopySharded(const wstring& src, const wstring& dst, int threads) {
    std::vector<wstring> dirs, files;
    EnumTree(src, L"", dirs, files);
    CreateDirectoryW(dst.c_str(), nullptr);
    for (auto& d : dirs) CreateDirectoryW((dst + L"\\" + d).c_str(), nullptr);

    // group files by their parent directory ("" = root)
    std::vector<std::vector<wstring>> shards;
    {
        wstring curDir = L"\x01";                    // impossible name -> first file opens a shard
        for (auto& f : files) {
            size_t cut = f.find_last_of(L'\\');
            wstring d = cut == wstring::npos ? L"" : f.substr(0, cut);
            if (d != curDir) { shards.emplace_back(); curDir = d; }
            shards.back().push_back(f);
        }
    }
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> ok{ true };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= shards.size()) return;
            for (auto& f : shards[i])
                if (!CopyFileW((src + L"\\" + f).c_str(), (dst + L"\\" + f).c_str(), FALSE))
                    ok = false;
        }
    });
    for (auto& t : pool) t.join();
    return ok;
}

// Sharded with a floor on parallelism: directories bigger than chunkFiles are
// split into chunks, so one huge directory still gets all threads (same-dir
// contention costs ~2x, but a single thread costs ~4x). Nested trees behave
// like pure dir-sharding because small dirs stay one chunk.
static bool NativeTreeCopyChunked(const wstring& src, const wstring& dst, int threads) {
    const size_t chunkFiles = 256;
    std::vector<wstring> dirs, files;
    EnumTree(src, L"", dirs, files);
    CreateDirectoryW(dst.c_str(), nullptr);
    for (auto& d : dirs) CreateDirectoryW((dst + L"\\" + d).c_str(), nullptr);

    std::vector<std::pair<size_t, size_t>> chunks;   // [begin, end) into files
    {
        wstring curDir = L"\x01";
        size_t start = 0;
        auto close = [&](size_t end) {
            for (size_t b = start; b < end; b += chunkFiles)
                chunks.emplace_back(b, std::min<size_t>(end, b + chunkFiles));
        };
        for (size_t i = 0; i < files.size(); ++i) {
            size_t cut = files[i].find_last_of(L'\\');
            wstring d = cut == wstring::npos ? L"" : files[i].substr(0, cut);
            if (d != curDir) { close(i); start = i; curDir = d; }
        }
        close(files.size());
    }
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> ok{ true };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= chunks.size()) return;
            for (size_t f = chunks[i].first; f < chunks[i].second; ++f)
                if (!CopyFileW((src + L"\\" + files[f]).c_str(),
                               (dst + L"\\" + files[f]).c_str(), FALSE)) ok = false;
        }
    });
    for (auto& t : pool) t.join();
    return ok;
}

// Single file, unbuffered overlapped ring: qd buffers in flight, each buffer
// cycles read -> write-at-same-offset -> next read. IOCP keys: 1=read, 2=write.
static bool OverlappedCopy(const wstring& src, const wstring& dst, DWORD chunk, int qd) {
    HANDLE hs = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (hs == INVALID_HANDLE_VALUE) die("overlapped: open src");
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hs, &sz)) die("overlapped: size");
    const uint64_t fileSize = (uint64_t)sz.QuadPart;
    const uint64_t alignedEnd = (fileSize + 4095) & ~4095ull;

    HANDLE hd = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (hd == INVALID_HANDLE_VALUE) die("overlapped: open dst");

    FILE_END_OF_FILE_INFO eof; eof.EndOfFile.QuadPart = (LONGLONG)alignedEnd;
    SetFileInformationByHandle(hd, FileEndOfFileInfo, &eof, sizeof(eof));

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp) die("overlapped: iocp");
    if (!CreateIoCompletionPort(hs, iocp, 1, 0)) die("overlapped: bind src");
    if (!CreateIoCompletionPort(hd, iocp, 2, 0)) die("overlapped: bind dst");

    struct Op { OVERLAPPED ov; uint8_t* buf; uint64_t off; DWORD len; };
    std::vector<Op> ops((size_t)qd);
    for (auto& o : ops) {
        o.buf = (uint8_t*)VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!o.buf) die("overlapped: buffer alloc");
    }

    uint64_t nextOff = 0;
    int inFlight = 0;
    auto issueRead = [&](Op& o) -> bool {
        if (nextOff >= alignedEnd) return false;
        o.off = nextOff;
        o.len = (DWORD)std::min<uint64_t>(chunk, alignedEnd - nextOff);
        nextOff += o.len;
        memset(&o.ov, 0, sizeof(o.ov));
        o.ov.Offset = (DWORD)(o.off & 0xFFFFFFFFull);
        o.ov.OffsetHigh = (DWORD)(o.off >> 32);
        if (!ReadFile(hs, o.buf, o.len, nullptr, &o.ov) && GetLastError() != ERROR_IO_PENDING)
            die("overlapped: ReadFile");
        return true;
    };
    for (auto& o : ops) if (issueRead(o)) ++inFlight;

    while (inFlight > 0) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* pov = nullptr;
        if (!GetQueuedCompletionStatus(iocp, &bytes, &key, &pov, INFINITE))
            die("overlapped: completion");
        Op& o = *(Op*)pov;                 // OVERLAPPED is the first member
        if (key == 1) {                    // read done -> write same offset
            DWORD wlen = (bytes + 4095) & ~4095u;
            memset(&o.ov, 0, sizeof(o.ov));
            o.ov.Offset = (DWORD)(o.off & 0xFFFFFFFFull);
            o.ov.OffsetHigh = (DWORD)(o.off >> 32);
            if (!WriteFile(hd, o.buf, wlen, nullptr, &o.ov) && GetLastError() != ERROR_IO_PENDING)
                die("overlapped: WriteFile");
        } else {                           // write done -> reuse buffer
            if (!issueRead(o)) --inFlight;
        }
    }
    CloseHandle(iocp); CloseHandle(hs); CloseHandle(hd);
    for (auto& o : ops) VirtualFree(o.buf, 0, MEM_RELEASE);

    if (alignedEnd != fileSize) {          // trim sector-rounded tail
        HANDLE ht = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (ht == INVALID_HANDLE_VALUE) die("overlapped: reopen for trim");
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)fileSize;
        SetFilePointerEx(ht, p, nullptr, FILE_BEGIN);
        SetEndOfFile(ht);
        CloseHandle(ht);
    }
    return true;
}

// Buffered writes land in the page cache and drain asynchronously — a timing
// that stops when the copy call returns measures RAM, not disk. Every big-file
// engine flushes the destination inside the timed region to level the field
// (for unbuffered engines this is a near-no-op).
static void FlushDest(const wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) die("flush: open dst");
    if (!FlushFileBuffers(h)) die("flush: FlushFileBuffers");
    CloseHandle(h);
}

// ---- scenario D engines: delete strategies ---------------------------------

// Mimics Delete.cpp today: single-threaded recursive delete.
static void DeleteTreeSeq(const wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTreeSeq(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Dir-sharded parallel delete: files deleted by a pool (one directory's files
// per chunk, same sharding insight as the copy pool), then dirs removed
// bottom-up single-threaded (RemoveDirectory of an emptied dir is cheap).
static void DeleteTreeSharded(const wstring& root, int threads) {
    std::vector<wstring> dirs, files;
    EnumTree(root, L"", dirs, files);

    std::vector<std::pair<size_t, size_t>> chunks;
    {
        auto dirOf = [](const wstring& p) {
            size_t cut = p.find_last_of(L'\\');
            return cut == wstring::npos ? wstring() : p.substr(0, cut);
        };
        size_t start = 0;
        wstring cur = files.empty() ? L"" : dirOf(files[0]);
        for (size_t i = 1; i <= files.size(); ++i) {
            bool cutHere = (i == files.size()) || (i - start >= 256);
            if (!cutHere) {
                wstring d = dirOf(files[i]);
                if (d != cur) { cutHere = true; cur = std::move(d); }
            }
            if (cutHere) { chunks.emplace_back(start, i); start = i; }
        }
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
        for (;;) {
            size_t c = next.fetch_add(1);
            if (c >= chunks.size()) return;
            for (size_t i = chunks[c].first; i < chunks[c].second; ++i)
                DeleteFileW((root + L"\\" + files[i]).c_str());
        }
    });
    for (auto& t : pool) t.join();
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
        RemoveDirectoryW((root + L"\\" + *it).c_str());
    RemoveDirectoryW(root.c_str());
}

// ---------------------------------------------------------------- verification

static bool SampleCompare(const wstring& a, const wstring& b, int samples) {
    uint64_t sa = FileSizeOf(a), sb = FileSizeOf(b);
    if (sa != sb || sa == ~0ull) return false;
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (ha == INVALID_HANDLE_VALUE || hb == INVALID_HANDLE_VALUE) return false;
    const DWORD SPAN = 64 * 1024;
    std::vector<uint8_t> ba(SPAN), bb(SPAN);
    bool ok = true;
    for (int i = 0; i < samples && ok; ++i) {
        uint64_t off = sa > SPAN ? Rand64() % (sa - SPAN) : 0;
        DWORD want = (DWORD)std::min<uint64_t>(SPAN, sa - off);
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)off;
        DWORD ra = 0, rb = 0;
        SetFilePointerEx(ha, p, nullptr, FILE_BEGIN);
        SetFilePointerEx(hb, p, nullptr, FILE_BEGIN);
        ok = ReadFile(ha, ba.data(), want, &ra, nullptr) &&
             ReadFile(hb, bb.data(), want, &rb, nullptr) &&
             ra == want && rb == want && memcmp(ba.data(), bb.data(), want) == 0;
    }
    CloseHandle(ha); CloseHandle(hb);
    return ok;
}

// -------------------------------------------------------------------- reporting

static void Report(const char* name, double secs, uint64_t bytes) {
    printf("  %-40s %9.1f ms   %8.1f MB/s\n", name, secs * 1000.0, (double)bytes / secs / 1e6);
    fflush(stdout);
}

// ------------------------------------------------------------------------ main

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: bench.exe <workdir> [--quick] [--keep] [--small N] [--big GiB] [--move GiB]\n");
        return 2;
    }
    wstring work = argv[1];
    size_t smallN = 10000;          // 64 KiB each
    size_t smallDirs = 100;
    uint64_t bigGiB = 8, moveGiB = 2;
    int itersA = 3, itersB = 2, itersC = 3;
    bool keep = false, runA = true, runB = true, runC = true, runD = true;
    for (int i = 2; i < argc; ++i) {
        wstring a = argv[i];
        if (a == L"--quick") { smallN = 500; bigGiB = 1; moveGiB = 1; itersA = itersB = itersC = 1; }
        else if (a == L"--keep") keep = true;
        else if (a == L"--only" && i + 1 < argc) {
            wstring which = argv[++i];
            runA = which == L"A"; runB = which == L"B"; runC = which == L"C";
            runD = which == L"D";
        }
        else if (a == L"--small" && i + 1 < argc) smallN = (size_t)_wtoi64(argv[++i]);
        else if (a == L"--dirs" && i + 1 < argc) smallDirs = (size_t)_wtoi64(argv[++i]);
        else if (a == L"--big" && i + 1 < argc) bigGiB = (uint64_t)_wtoi64(argv[++i]);
        else if (a == L"--move" && i + 1 < argc) moveGiB = (uint64_t)_wtoi64(argv[++i]);
        else { fprintf(stderr, "unknown arg\n"); return 2; }
    }
    const uint64_t SMALL_FILE = 64 * 1024;
    const uint64_t smallBytes = smallN * SMALL_FILE;
    const uint64_t bigBytes = bigGiB << 30;
    const uint64_t moveBytes = moveGiB << 30;

    InitRandomBuf();
    CreateDirectoryW(work.c_str(), nullptr);

    ULARGE_INTEGER freeBytes{};
    GetDiskFreeSpaceExW(work.c_str(), &freeBytes, nullptr, nullptr);
    uint64_t need = bigBytes * 2 + moveBytes * 2 + smallBytes * 2 + (1ull << 30);
    printf("workdir: %ls\nfree: %.1f GiB, needed: %.1f GiB\n",
           work.c_str(), freeBytes.QuadPart / 1073741824.0, need / 1073741824.0);
    if (freeBytes.QuadPart < need) { fprintf(stderr, "not enough free space\n"); return 3; }

    wstring srcSmall = work + L"\\srcSmall", dstSmall = work + L"\\dstSmall";
    wstring srcBig = work + L"\\srcBig",     dstBig = work + L"\\dstBig";
    wstring srcMove = work + L"\\srcMove",   dstMove = work + L"\\dstMove";
    wstring bigSrcFile = srcBig + L"\\big.bin", bigDstFile = dstBig + L"\\big.bin";
    wstring mvSrcFile = srcMove + L"\\mv.bin",  mvDstFile = dstMove + L"\\mv.bin";

    // ---- prepare datasets (reused if already present with matching shape)
    printf("\npreparing data ...\n"); fflush(stdout);
    bool shapeOk = false;
    if (runA || runD) {
        std::vector<wstring> dd, ff;
        EnumTree(srcSmall, L"", dd, ff);
        shapeOk = ff.size() == smallN && dd.size() == smallDirs;
    }
    if ((runA || runD) && !shapeOk) {
        RemoveTree(srcSmall);
        CreateDirectoryW(srcSmall.c_str(), nullptr);
        size_t nDirs = smallDirs, perDir = (smallN + nDirs - 1) / nDirs, made = 0;
        for (size_t d = 0; d < nDirs && made < smallN; ++d) {
            wchar_t sub[32]; swprintf(sub, 32, L"\\d%03zu", d);
            wstring dir = srcSmall + sub;
            CreateDirectoryW(dir.c_str(), nullptr);
            for (size_t f = 0; f < perDir && made < smallN; ++f, ++made) {
                wchar_t fn[32]; swprintf(fn, 32, L"\\f%05zu.bin", f);
                WriteRandomFile(dir + fn, SMALL_FILE);
            }
        }
    }
    CreateDirectoryW(srcBig.c_str(), nullptr);
    if (runB && FileSizeOf(bigSrcFile) != bigBytes) WriteRandomFile(bigSrcFile, bigBytes);
    CreateDirectoryW(srcMove.c_str(), nullptr);
    CreateDirectoryW(dstMove.c_str(), nullptr);
    printf("data ready: %zu small files (%.0f MiB), big %.0f MiB, move %.0f MiB\n\n",
           smallN, smallBytes / 1048576.0, bigBytes / 1048576.0, moveBytes / 1048576.0);
    fflush(stdout);

    bool ok = false;

    // ---- scenario A: many small files -------------------------------------
    if (runA) {
    printf("== A: %zu x 64 KiB files, cold cache ==\n", smallN);
    for (int it = 0; it < itersA; ++it) {
        RemoveTree(dstSmall);
        EvictTree(srcSmall);
        double t = RunRobocopy(L"\"" + srcSmall + L"\" \"" + dstSmall + L"\" /E /MT:64 " + RQUIET, &ok);
        if (!ok) die("A: robocopy failed");
        if (CountTree(dstSmall) != smallN) die("A: robocopy wrong file count");
        Report("robocopy /E /MT:64", t, smallBytes);

        struct AEngine {
            const char* name;
            bool (*run)(const wstring&, const wstring&, int);
            int threads;
        } aengines[] = {
            { "native interleaved, 16 thr",   NativeTreeCopy,        16 },
            { "native dir-sharded, 16 thr",   NativeTreeCopySharded, 16 },
            { "native chunked,     16 thr",   NativeTreeCopyChunked, 16 },
        };
        for (auto& e : aengines) {
            RemoveTree(dstSmall);
            EvictTree(srcSmall);
            double t0 = Now();
            if (!e.run(srcSmall, dstSmall, e.threads)) die("A: native copy failed");
            t = Now() - t0;
            if (CountTree(dstSmall) != smallN) die("A: native wrong file count");
            Report(e.name, t, smallBytes);
        }
    }
    }

    // ---- scenario B: one big file -----------------------------------------
    if (runB) {
    printf("\n== B: one %.0f GiB file, cold cache ==\n", (double)bigGiB);
    for (int it = 0; it < itersB; ++it) {
        auto fresh = [&] { RemoveTree(dstBig); CreateDirectoryW(dstBig.c_str(), nullptr); EvictFile(bigSrcFile); };
        auto check = [&](const char* who) {
            if (FileSizeOf(bigDstFile) != bigBytes) die("B: wrong size");
            if (it == 0 && !SampleCompare(bigSrcFile, bigDstFile, 16)) {
                fprintf(stderr, "FATAL: B: %s produced corrupt data\n", who); exit(1);
            }
        };

        fresh();
        double t0 = Now();
        RunRobocopy(L"\"" + srcBig + L"\" \"" + dstBig + L"\" big.bin /MT:64 " + RQUIET, &ok);
        if (!ok) die("B: robocopy failed");
        FlushDest(bigDstFile);
        double t = Now() - t0;
        check("robocopy"); Report("robocopy /MT:64 (+flush)", t, bigBytes);

        fresh();
        t0 = Now();
        RunRobocopy(L"\"" + srcBig + L"\" \"" + dstBig + L"\" big.bin /MT:64 /J " + RQUIET, &ok);
        if (!ok) die("B: robocopy /J failed");
        FlushDest(bigDstFile);
        t = Now() - t0;
        check("robocopy /J"); Report("robocopy /MT:64 /J (+flush)", t, bigBytes);

        fresh();
        t0 = Now();
        if (!CopyFileW(bigSrcFile.c_str(), bigDstFile.c_str(), FALSE)) die("B: CopyFileW failed");
        FlushDest(bigDstFile);
        t = Now() - t0;
        check("CopyFileW"); Report("native CopyFileW buffered (+flush)", t, bigBytes);

        fresh();
        BOOL cancel = FALSE;
        t0 = Now();
        if (!CopyFileExW(bigSrcFile.c_str(), bigDstFile.c_str(), nullptr, nullptr, &cancel,
                         COPY_FILE_NO_BUFFERING)) die("B: CopyFileEx failed");
        FlushDest(bigDstFile);
        t = Now() - t0;
        check("CopyFileEx"); Report("native CopyFileEx NO_BUFFERING (+flush)", t, bigBytes);

        fresh();
        t0 = Now();
        OverlappedCopy(bigSrcFile, bigDstFile, 8u << 20, 8);
        FlushDest(bigDstFile);
        t = Now() - t0;
        check("overlapped 8x8"); Report("native overlapped QD8 x 8 MiB (+flush)", t, bigBytes);

        fresh();
        t0 = Now();
        OverlappedCopy(bigSrcFile, bigDstFile, 16u << 20, 16);
        FlushDest(bigDstFile);
        t = Now() - t0;
        check("overlapped 16x16"); Report("native overlapped QD16 x 16 MiB (+flush)", t, bigBytes);
    }
    }

    // ---- scenario C: same-volume move -------------------------------------
    if (runC) {
    printf("\n== C: %.0f GiB same-volume move ==\n", (double)moveGiB);
    for (int it = 0; it < itersC; ++it) {
        if (FileSizeOf(mvSrcFile) != moveBytes) WriteRandomFile(mvSrcFile, moveBytes);
        DeleteFileW(mvDstFile.c_str());
        EvictFile(mvSrcFile);
        double t0c = Now();
        RunRobocopy(L"\"" + srcMove + L"\" \"" + dstMove + L"\" mv.bin /MOV " + RQUIET, &ok);
        if (!ok) die("C: robocopy /MOV failed");
        FlushDest(mvDstFile);
        double t = Now() - t0c;
        if (FileSizeOf(mvDstFile) != moveBytes) die("C: robocopy move wrong size");
        Report("robocopy /MOV (+flush)", t, moveBytes);

        WriteRandomFile(mvSrcFile, moveBytes);      // robocopy deleted the source
        DeleteFileW(mvDstFile.c_str());
        EvictFile(mvSrcFile);
        double t0 = Now();
        if (!MoveFileExW(mvSrcFile.c_str(), mvDstFile.c_str(), MOVEFILE_REPLACE_EXISTING))
            die("C: MoveFileExW failed");
        t = Now() - t0;
        if (FileSizeOf(mvDstFile) != moveBytes) die("C: native move wrong size");
        Report("native MoveFileExW (rename)", t, moveBytes);
        DeleteFileW(mvDstFile.c_str());             // next iter recreates the source
    }
    }

    // ---- scenario D: delete a many-file tree ------------------------------
    if (runD) {
    printf("\n== D: delete a %zu-file tree ==\n", smallN);
    struct DEng { const char* name; int threads; } dengs[] = {
        { "sequential recursive (Delete.cpp today)", 0 },
        { "dir-sharded pool,  4 thr", 4 },
        { "dir-sharded pool,  8 thr", 8 },
        { "dir-sharded pool, 16 thr", 16 },
    };
    wstring delWork = work + L"\\delWork";
    for (int it = 0; it < itersA; ++it) {
        for (auto& e : dengs) {
            RemoveTree(delWork);
            NativeTreeCopySharded(srcSmall, delWork, 16); // fresh victim tree
            double t0 = Now();
            if (e.threads == 0) DeleteTreeSeq(delWork);
            else DeleteTreeSharded(delWork, e.threads);
            double t = Now() - t0;
            if (GetFileAttributesW(delWork.c_str()) != INVALID_FILE_ATTRIBUTES)
                die("D: tree not fully deleted");
            printf("  %-42s %9.1f ms   %8.0f files/s\n", e.name, t * 1000.0,
                   (double)smallN / t);
            fflush(stdout);
        }
    }
    }

    if (!keep) {
        printf("\ncleaning up ...\n");
        RemoveTree(work);
    } else {
        printf("\n--keep: data left in %ls\n", work.c_str());
    }
    printf("done.\n");
    return 0;
}
