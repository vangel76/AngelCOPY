#include "ProgressUI.h"
#include "Delete.h"
#include "NativeCopy.h"
#include "../shared/Localize.h"
#include "../shared/Theme.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shobjidl.h>   // ITaskbarList3
#include <strsafe.h>
#include <cwctype>
#include <deque>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")

// Pull in ComCtl32 v6 so the progress bar / button render themed (modern look).
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

namespace angelcopy {

namespace {

constexpr UINT WM_APP_DONE = WM_APP + 1;

// Layout is defined in CLIENT coordinates; the window size is derived with
// AdjustWindowRectEx. Mixing the two is what previously let the report box
// overlap the Close button.
constexpr int UI_W = 480;      // client width
constexpr int UI_MARGIN = 16;
constexpr int UI_BTN_W = 84, UI_BTN_H = 28;

// Windows' own copy dialog draws the progress and the throughput graph as ONE
// control: a solid band grows left→right for progress, the speed curve is drawn
// over it. Same here — hence no msctls_progress32 any more.
constexpr int UI_CHART_Y = 38, UI_CHART_H = 96;
constexpr int UI_LINE1_Y = 146; // Name:
constexpr int UI_LINE2_Y = 166; // Time remaining:
constexpr int UI_LINE3_Y = 186; // Items remaining:
constexpr int UI_BTN_Y = 214;
constexpr int UI_CH_NORMAL = UI_BTN_Y + UI_BTN_H + UI_MARGIN;                 // 258

// Report mode: the error/skip box appears under the info lines.
constexpr int UI_BOX_Y = 214;
constexpr int UI_BOX_H = 150;
constexpr int UI_BTN_Y_REPORT = UI_BOX_Y + UI_BOX_H + 12;                     // 376
constexpr int UI_CH_REPORT = UI_BTN_Y_REPORT + UI_BTN_H + UI_MARGIN;          // 420

// The chart's X axis is PROGRESS, not time — same as Explorer's copy dialog.
// The graph and the bar are the same thing: each throughput sample is stored in
// the bucket for the percentage reached at that moment, so the curve's right
// edge is always exactly the progress position, and the axis is fixed at
// 0..100% of the transfer. Nothing ever compresses or scrolls.
constexpr int CHART_BUCKETS = 480;            // resolution across 0..100%
constexpr ULONGLONG CHART_SAMPLE_MS = 150;    // how often a bucket is written

// Size the window so its CLIENT area is exactly cw x ch.
void SetClientSize(HWND hwnd, int cw, int ch) {
    RECT r{0, 0, cw, ch};
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

// Shared state between the worker thread and the UI thread.
struct Shared {
    CRITICAL_SECTION cs;
    unsigned long long totalBytes = 0;     // expected + skipped: the bar's 100%
    unsigned long long totalRealBytes = 0; // expected only: scales speed and ETA
    unsigned long long totalFiles = 0;
    unsigned long long doneBytes = 0;
    unsigned long long doneFiles = 0;
    // Bytes robocopy listed but did not copy (identical / policy-excluded).
    // They advance the bar — painted green — but never the speed or the ETA.
    unsigned long long skipBytes = 0;
    // Lowercased source paths the scan says robocopy will skip (SkipSetFor).
    // /V lists skipped files exactly like copied ones (/NC drops the class
    // word, which is localized anyway), so the path is the only discriminator.
    const std::unordered_set<std::wstring>* skipSet = nullptr;
    // Lowercased destination roots. robocopy also lists files that exist only
    // at the DESTINATION ("extra" files) and /NC makes those lines identical to
    // copy lines too — but their paths are under a destination root, ours never
    // are. Without this filter every extra file inflated the file counter.
    std::vector<std::wstring> dstPrefixes;
    std::wstring currentFile;
    std::wstring destLabel;
    std::vector<std::wstring> errors; // robocopy ERROR lines (capped)
    bool    expectErrDetail = false;  // engine-thread only: next line is detail
    HANDLE  hCurrentProc = nullptr;   // current robocopy, for cancel + IO counters

    // Byte progress source. robocopy's stdout is a pipe, so its CRT buffers in
    // 4 KB blocks: a single huge file produces ~100 bytes of output and we
    // receive NOTHING until the process exits — the dialog sat at 0 B / 0 B/s
    // for the whole copy (verified with an 8 GB file). Polling the destination
    // file size does not help either: robocopy pre-allocates it to full size
    // immediately. So for robocopy we take the bytes from the process's own IO
    // counters instead, which are live regardless of buffering. The delete path
    // has no child process and keeps reporting bytes directly.
    bool    bytesFromIo = false;
    unsigned long long ioCompleted = 0; // write bytes of already-finished jobs
    // Mirror only: set once the copy phase is done and extras are being
    // deleted. The UI switches its heading; byte progress stays on the copy
    // volume (deletion is metadata work, not transfer).
    bool    phaseDelete = false;
    LONG    cancel = 0;               // set by Cancel button
    bool    finished = false;
    int     exitCode = 0;

    Shared()  { InitializeCriticalSection(&cs); }
    ~Shared() { DeleteCriticalSection(&cs); }
};

// The dialog is engine-agnostic: it runs whatever worker it is handed, so the
// copy (robocopy) and delete (own recursive deleter) paths share one window.
// The worker returns the worst exit code (>=8 == failure, robocopy convention).
using Worker = std::function<int(Shared&)>;

struct EngineArgs {
    Shared* sh;
    HWND hwnd;
    Worker worker;
};

// ---- output parsing ------------------------------------------------------

std::wstring OemToWide(const char* data, size_t len) {
    if (len == 0) return L"";
    int n = MultiByteToWideChar(CP_OEMCP, 0, data, (int)len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, data, (int)len, &w[0], n);
    return w;
}

bool AllDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
    return true;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring LowerCopy(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

bool HasPrefix(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

// Parse one robocopy output line. A file line under /BYTES /FP /NC looks like
// "\t  \t\t<bytes>\t<fullpath>": tab-separated with a pure-digit size field and
// a path field containing a drive/UNC. Returns true and fills size/path then.
bool ParseFileLine(const std::wstring& line, unsigned long long& size,
                   std::wstring& path) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (true) {
        size_t tab = line.find(L'\t', start);
        std::wstring f =
            line.substr(start, tab == std::wstring::npos ? std::wstring::npos
                                                         : tab - start);
        f = Trim(f);
        if (!f.empty()) fields.push_back(f);
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    if (fields.size() < 2) return false;

    const std::wstring& last = fields.back();
    bool looksPath = last.size() >= 2 &&
                     ((last[1] == L':') || (last[0] == L'\\' && last[1] == L'\\'));
    if (!looksPath) return false;

    for (size_t i = 0; i + 1 < fields.size(); ++i) {
        if (AllDigits(fields[i])) {
            size = _wcstoui64(fields[i].c_str(), nullptr, 10);
            path = last;
            return true;
        }
    }
    return false;
}

void FeedChunk(Shared* sh, std::string& pending, const char* buf, DWORD n) {
    pending.append(buf, n);
    // Split on either CR or LF (robocopy uses CR between percentage updates).
    size_t pos;
    while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
        std::string raw = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        if (raw.empty()) continue;
        std::wstring line = OemToWide(raw.data(), raw.size());

        unsigned long long size = 0;
        std::wstring path;
        if (ParseFileLine(line, size, path)) {
            std::wstring lower = LowerCopy(path);

            // "Extra" files (present only at the destination) are listed with
            // their DESTINATION path; robocopy never touches them and neither
            // must the progress counters.
            bool isExtra = false;
            for (const auto& p : sh->dstPrefixes)
                if (HasPrefix(lower, p)) { isExtra = true; break; }
            if (isExtra) continue;

            bool isSkip = sh->skipSet && sh->skipSet->count(lower) > 0;
            EnterCriticalSection(&sh->cs);
            if (isSkip) {
                // Listed but not copied: advances the (green) bar only. Real
                // bytes keep coming from the IO counters.
                sh->skipBytes += size;
            } else if (!sh->bytesFromIo) {
                // Bytes come from the IO counters when a child process owns
                // them; adding the line size too would double-count.
                sh->doneBytes += size;
            }
            sh->doneFiles += 1;
            sh->currentFile = path;
            sh->expectErrDetail = false;
            LeaveCriticalSection(&sh->cs);
        } else if (line.find(L"(0x") != std::wstring::npos) {
            // robocopy error line. Detect the Win32 error-code marker "(0x...)"
            // instead of the word "ERROR" — robocopy is localized (e.g. German
            // prints "FEHLER"), but the "(0xNNNNNNNN)" code is language-neutral.
            // Example: "... FEHLER 32 (0x00000020) ... <path>".
            EnterCriticalSection(&sh->cs);
            if (sh->errors.size() < 200) sh->errors.push_back(Trim(line));
            sh->expectErrDetail = true;
            LeaveCriticalSection(&sh->cs);
        } else {
            // The line right after an ERROR is its human description; append it.
            std::wstring t = Trim(line);
            EnterCriticalSection(&sh->cs);
            if (sh->expectErrDetail && !t.empty() && !sh->errors.empty()) {
                sh->errors.back() += L"  \x2014  " + t;
                sh->expectErrDetail = false;
            }
            LeaveCriticalSection(&sh->cs);
        }
        // Percentage lines are suppressed via /NP; nothing else to parse.
    }
}

// ---- worker thread -------------------------------------------------------

// Bytes copied so far: finished jobs plus the live counter of the running one.
// Each job is its own process, so the counters restart per job and have to be
// accumulated.
unsigned long long DoneBytes(Shared& sh) {
    EnterCriticalSection(&sh.cs);
    unsigned long long total;
    if (sh.bytesFromIo) {
        total = sh.ioCompleted;
        IO_COUNTERS ic{};
        if (sh.hCurrentProc && GetProcessIoCounters(sh.hCurrentProc, &ic))
            total += ic.WriteTransferCount;
    } else {
        total = sh.doneBytes;
    }
    LeaveCriticalSection(&sh.cs);
    return total;
}

// Worker: drive robocopy over every job, parsing its piped output.
int RunRobocopyJobs(Shared& shared, Operation op,
                    const std::vector<RoboJob>& jobs, Conflict policy) {
    Shared* sh = &shared;
    std::wstring exe = RobocopyExe();
    int worst = 0;

    EnterCriticalSection(&sh->cs);
    sh->bytesFromIo = true; // a child process owns the bytes; see Shared
    LeaveCriticalSection(&sh->cs);

    for (const RoboJob& job : jobs) {
        if (InterlockedCompareExchange(&sh->cancel, 0, 0)) break;

        std::wstring args =
            BuildRobocopyArgs(op, job, /*parseable=*/true, policy);
        std::wstring cmd = L"\"" + exe + L"\" " + args;

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        // 256 KB buffer so robocopy's 64 threads rarely block waiting on us.
        if (!CreatePipe(&rd, &wr, &sa, 256 * 1024)) { worst = 16; continue; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mut(cmd.begin(), cmd.end());
        mut.push_back(L'\0');

        BOOL ok = CreateProcessW(exe.c_str(), mut.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr); // our write end; child owns its copy
        if (!ok) { CloseHandle(rd); worst = 16; continue; }

        EnterCriticalSection(&sh->cs);
        sh->hCurrentProc = pi.hProcess;
        LeaveCriticalSection(&sh->cs);

        std::string pending;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
            FeedChunk(sh, pending, buf, got);
        CloseHandle(rd);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);

        // Bank this job's final byte count before the handle goes away, then
        // clear the handle under the lock so the UI never reads a closed one.
        EnterCriticalSection(&sh->cs);
        IO_COUNTERS ic{};
        if (GetProcessIoCounters(pi.hProcess, &ic))
            sh->ioCompleted += ic.WriteTransferCount;
        sh->hCurrentProc = nullptr;
        LeaveCriticalSection(&sh->cs);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if ((int)code > worst) worst = (int)code;
    }
    return worst;
}

// Worker: the native engine, feeding Shared directly. No child process, so no
// IO-counter detour (bytesFromIo stays false), no pipe parsing, no skip-set
// path matching — the engine reports bytes, files, skips and errors exactly.
int RunNativeUiJobs(Shared& sh, Operation op, const std::vector<RoboJob>& jobs,
                    Conflict policy) {
    CopySink sink;
    sink.onBytes = [&sh](unsigned long long d) {
        EnterCriticalSection(&sh.cs);
        sh.doneBytes += d;
        LeaveCriticalSection(&sh.cs);
    };
    sink.onFileStart = [&sh](const std::wstring& p) {
        EnterCriticalSection(&sh.cs);
        sh.currentFile = p;
        LeaveCriticalSection(&sh.cs);
    };
    sink.onFileDone = [&sh](const std::wstring&, unsigned long long) {
        EnterCriticalSection(&sh.cs);
        sh.doneFiles += 1;
        LeaveCriticalSection(&sh.cs);
    };
    sink.onSkip = [&sh](const std::wstring& p, unsigned long long size) {
        EnterCriticalSection(&sh.cs);
        sh.skipBytes += size;
        sh.doneFiles += 1;
        sh.currentFile = p;
        LeaveCriticalSection(&sh.cs);
    };
    sink.onError = [&sh](const std::wstring& msg) {
        EnterCriticalSection(&sh.cs);
        if (sh.errors.size() < 200) sh.errors.push_back(msg);
        LeaveCriticalSection(&sh.cs);
    };
    sink.cancelled = [&sh] {
        return InterlockedCompareExchange(&sh.cancel, 0, 0) != 0;
    };
    return RunNativeJobs(op, jobs, policy, sink);
}

// Copy worker dispatch: native engine unless ANGELCOPY_ENGINE=robocopy.
int RunCopyJobs(Shared& sh, Operation op, const std::vector<RoboJob>& jobs,
                Conflict policy) {
    return UseNativeEngine() ? RunNativeUiJobs(sh, op, jobs, policy)
                             : RunRobocopyJobs(sh, op, jobs, policy);
}

// Worker: permanent recursive delete, reporting into the same Shared state the
// robocopy path uses, so the dialog shows progress and errors identically.
int RunDelete(Shared& sh, const std::vector<std::wstring>& targets) {
    DeleteSink sink;
    sink.onFile = [&sh](const std::wstring& path, unsigned long long size) {
        EnterCriticalSection(&sh.cs);
        sh.doneBytes += size;
        sh.doneFiles += 1;
        sh.currentFile = path;
        LeaveCriticalSection(&sh.cs);
    };
    sink.onError = [&sh](const std::wstring& msg) {
        EnterCriticalSection(&sh.cs);
        if (sh.errors.size() < 200) sh.errors.push_back(msg);
        LeaveCriticalSection(&sh.cs);
    };
    sink.cancelled = [&sh] {
        return InterlockedCompareExchange(&sh.cancel, 0, 0) != 0;
    };
    return DeleteTargets(targets, sink);
}

DWORD WINAPI EngineThread(LPVOID param) {
    EngineArgs* ea = static_cast<EngineArgs*>(param);
    int worst = ea->worker(*ea->sh);

    EnterCriticalSection(&ea->sh->cs);
    ea->sh->finished = true;
    ea->sh->exitCode = worst;
    LeaveCriticalSection(&ea->sh->cs);
    PostMessageW(ea->hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

// ---- formatting ----------------------------------------------------------

std::wstring HumanBytes(unsigned long long b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t out[64];
    StringCchPrintfW(out, 64, (v < 10 && i > 0) ? L"%.1f %s" : L"%.0f %s", v, u[i]);
    return out;
}

std::wstring FormatEta(double sec) {
    if (sec < 0 || sec > 359999) return L"--:--";
    int s = (int)(sec + 0.5);
    wchar_t out[32];
    if (s >= 3600)
        StringCchPrintfW(out, 32, L"%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    else
        StringCchPrintfW(out, 32, L"%d:%02d", s / 60, s % 60);
    return out;
}

// ---- window --------------------------------------------------------------

struct UiState {
    Shared* sh;
    HWND hwnd;   // top-level window, for the taskbar progress button
    HWND chart, lblTitle, lblName, lblEta, lblRemaining, btnCancel, edErrors;
    HFONT font, fontBold;
    ITaskbarList3* taskbar = nullptr; // colored taskbar-button progress; may be null
    bool deleteMode = false;          // rate/label in items/sec, not bytes/sec
    bool itemsRateNow = false;        // effective this tick (delete or purge phase)
    ULONGLONG startTick;
    // (tick, doneBytes) history for the sliding-window rate.
    std::deque<std::pair<ULONGLONG, unsigned long long>> hist;
    bool done;
    SkipInfo skipped;
    Conflict policy;

    // --- chart: one throughput bucket per slice of progress ---
    std::vector<float> lane = std::vector<float>(CHART_BUCKETS, 0.0f);
    // 1 where that stretch of progress was skipped files, not copied bytes —
    // painted green, and the throughput curve is not drawn across it.
    std::vector<unsigned char> skipLane =
        std::vector<unsigned char>(CHART_BUCKETS, 0);
    int laneMax = -1;                // highest bucket written so far
    ULONGLONG lastSampleTick = 0;
    unsigned long long lastSampleReal = 0; // copied bytes at the last sample
    unsigned long long lastSampleSkip = 0; // skipped bytes at the last sample
    unsigned long long lastSampleFiles = 0; // done files at the last sample (delete)
    int pct = 0;                     // drives the progress band AND the curve end
    double rateNow = 0;              // smoothed, for the in-chart label
};

// Record one interval at the current percentage. Progress only moves forward,
// so buckets fill left→right; when progress jumps the crossed buckets are
// filled rather than left as a gap. The interval's advance splits into a
// copied part (dReal, gets the throughput value) and a skipped part (dSkip,
// marked green with no throughput): the crossed buckets are divided in that
// proportion. Within one 150 ms tick the ordering is unknowable anyway —
// /MT:64 interleaves both — so green-first inside the range is as true as
// anything else.
void RecordSample(UiState* ui, int pct, double bytesPerSec,
                  unsigned long long dReal, unsigned long long dSkip) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int idx = pct * (CHART_BUCKETS - 1) / 100;
    if (idx < ui->laneMax) idx = ui->laneMax; // never walk backwards
    // The very first sample covers everything since 0% — starting it at the
    // current bucket instead would leave the stretch left of it unclassified
    // (an all-skip run that finishes within one interval stayed blue).
    int from = (ui->laneMax < 0) ? 0 : ui->laneMax;
    int count = idx - from + 1;

    int nSkip = 0;
    if (dSkip > 0) {
        nSkip = (dReal == 0) ? count
                             : (int)((double)count * (double)dSkip /
                                     (double)(dSkip + dReal) + 0.5);
        if (nSkip > count) nSkip = count;
    }
    for (int i = from; i <= idx; ++i) {
        bool skip = (i - from) < nSkip;
        ui->skipLane[i] = skip ? 1 : 0;
        ui->lane[i] = skip ? 0.0f : (float)bytesPerSec;
    }
    ui->laneMax = idx;
}

void SetChildFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

// ---- chart: progress band + throughput curve in one control ----------------

void PaintChart(HWND hwnd, UiState* ui) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const theme::Colors& c = theme::C();

    // Draw off-screen and blit once: painting straight to the DC flickers badly
    // at a 100 ms refresh.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(c.chartBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // The live progress position in buckets; buckets past laneMax exist only
    // because the band is ahead of the newest sample — they inherit its state.
    int idxNow = ui->pct * (CHART_BUCKETS - 1) / 100;
    if (idxNow < ui->laneMax) idxNow = ui->laneMax;
    auto skipAt = [ui](int i) {
        if (ui->laneMax < 0) return false;
        return ui->skipLane[i <= ui->laneMax ? i : ui->laneMax] != 0;
    };

    // Progress band — this is the "bar" half of the control. Skipped stretches
    // are painted green over the base fill: those parts of the transfer were
    // never copied, only found to be already there.
    if (ui->pct > 0) {
        const int bandRight = w * ui->pct / 100;
        RECT pr{0, 0, bandRight, h};
        HBRUSH b = CreateSolidBrush(c.chartFill);
        FillRect(mem, &pr, b);
        DeleteObject(b);

        HBRUSH sk = CreateSolidBrush(c.chartSkip);
        int i = 0;
        while (i <= idxNow) {
            if (!skipAt(i)) { ++i; continue; }
            int a = i;
            while (i <= idxNow && skipAt(i)) ++i;
            int left = (int)((long long)a * w / (CHART_BUCKETS - 1));
            int right = (i > idxNow)
                            ? bandRight // run reaches the live edge: stay flush
                            : (int)((long long)i * w / (CHART_BUCKETS - 1));
            if (right > left) {
                RECT sr{left, 0, right, h};
                FillRect(mem, &sr, sk);
            }
        }
        DeleteObject(sk);
    }

    HPEN gridPen = CreatePen(PS_SOLID, 1, c.chartGrid);
    HGDIOBJ oldPen = SelectObject(mem, gridPen);
    for (int i = 1; i < 4; ++i) {
        int y = h * i / 4;
        MoveToEx(mem, 0, y, nullptr);
        LineTo(mem, w, y);
    }
    for (int i = 1; i < 8; ++i) {
        int x = w * i / 8;
        MoveToEx(mem, x, 0, nullptr);
        LineTo(mem, x, h);
    }
    SelectObject(mem, oldPen);
    DeleteObject(gridPen);

    // Throughput curve. X is progress: bucket i sits at i/(CHART_BUCKETS-1) of
    // the width, so the curve ends exactly at the progress band's edge and the
    // axis stays fixed at 0..100% for the whole transfer. Skipped stretches
    // carry no throughput, so the curve breaks there — green stays bare.
    double peak = 0;
    for (int i = 0; i <= ui->laneMax; ++i)
        if (ui->lane[i] > peak) peak = ui->lane[i];

    if (ui->laneMax >= 1 && peak > 0) {
        const double scale = peak * 1.15; // headroom so the peak isn't clipped
        // Carry the last rate up to the live progress position: the newest
        // sample can be one interval old, and without this the curve visibly
        // trails the band it is supposed to be part of.
        HBRUSH ab = CreateSolidBrush(c.chartArea);
        HPEN cp = CreatePen(PS_SOLID, 2, c.chartCurve);
        HPEN np = (HPEN)GetStockObject(NULL_PEN);

        int i = 0;
        while (i <= idxNow) {
            if (skipAt(i)) { ++i; continue; }
            int a = i;
            std::vector<POINT> pts;
            while (i <= idxNow && !skipAt(i)) {
                float v = (i <= ui->laneMax) ? ui->lane[i]
                                             : ui->lane[ui->laneMax];
                int x = (int)((long long)i * w / (CHART_BUCKETS - 1));
                int y = h - (int)(v / scale * h);
                if (y < 0) y = 0;
                if (y > h) y = h;
                pts.push_back(POINT{x, y});
                ++i;
            }
            (void)a;
            if (pts.size() < 2) continue; // a 1-bucket sliver draws nothing

            // Filled area under this segment, then its line on top.
            std::vector<POINT> area = pts;
            area.push_back(POINT{pts.back().x, h});
            area.push_back(POINT{pts.front().x, h});
            HGDIOBJ ob = SelectObject(mem, ab);
            HGDIOBJ op2 = SelectObject(mem, np);
            Polygon(mem, area.data(), (int)area.size());
            SelectObject(mem, ob);
            SelectObject(mem, op2);

            HGDIOBJ ocp = SelectObject(mem, cp);
            Polyline(mem, pts.data(), (int)pts.size());
            SelectObject(mem, ocp);
        }
        DeleteObject(ab);
        DeleteObject(cp);
    }

    // Speed label, inside the chart at the top right (as Windows does).
    // Bytes/sec normally, items/sec during a delete/purge.
    wchar_t speed[96];
    if (ui->itemsRateNow)
        StringCchPrintfW(speed, 96, loc::T(loc::S::ChartSpeedItems),
                         (unsigned long long)ui->rateNow);
    else
        StringCchPrintfW(speed, 96, loc::T(loc::S::ChartSpeed),
                         HumanBytes((unsigned long long)ui->rateNow).c_str());
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, c.text);
    HGDIOBJ oldFont = SelectObject(mem, ui->font);
    RECT tr{0, 6, w - 8, 24};
    DrawTextW(mem, speed, -1, &tr, DT_RIGHT | DT_SINGLELINE | DT_TOP);
    SelectObject(mem, oldFont);

    HPEN bp = CreatePen(PS_SOLID, 1, c.border);
    HGDIOBJ obp = SelectObject(mem, bp);
    HGDIOBJ obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, w, h);
    SelectObject(mem, obp);
    SelectObject(mem, obr);
    DeleteObject(bp);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UiState* ui = (UiState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // PaintChart covers every pixel; erasing first only flickers
    case WM_PAINT:
        if (ui) {
            PaintChart(hwnd, ui);
        } else {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void UpdateUi(UiState* ui) {
    Shared* sh = ui->sh;
    unsigned long long realDone = DoneBytes(*sh); // takes the lock itself
    EnterCriticalSection(&sh->cs);
    unsigned long long total = sh->totalBytes;
    unsigned long long totalReal = sh->totalRealBytes;
    unsigned long long skipDone = sh->skipBytes;
    unsigned long long dFiles = sh->doneFiles, tFiles = sh->totalFiles;
    std::wstring cur = sh->currentFile;
    bool phaseDel = sh->phaseDelete;
    LeaveCriticalSection(&sh->cs);

    // Copied and skipped bytes are kept apart the whole way down: both advance
    // the bar (skips as the green stretches), but speed and ETA are computed
    // from copied bytes ONLY — a skipped file moves no data, and letting it
    // into the rate is what made the graph spike absurdly on re-copies.
    if (realDone > totalReal) realDone = totalReal;
    unsigned long long done = realDone + skipDone;
    if (done > total) done = total;

    // Transfer rate over a sliding ~3s window.
    //
    // Neither extreme works here. A per-tick rate spikes wildly, because
    // robocopy prints a file's line when it *starts* the file and /MT:64
    // announces many at once, so the byte counter advances in bursts. But the
    // average since start is permanently too low: it includes robocopy's
    // startup and directory enumeration, seconds during which no bytes flow —
    // that is why it read about half of what Task Manager showed.
    //
    // A multi-second window averages the bursts out while still tracking the
    // current rate. Once finished, the whole-transfer average is the meaningful
    // summary (a "current" rate at completion is nothing).
    ULONGLONG now = GetTickCount64();
    double elapsed = (now - ui->startTick) / 1000.0;

    // Deletion is NTFS-metadata work: bytes/sec is meaningless there (it would
    // read "12 GB/s" one tick, "0" the next). The rate, ETA and the curve run
    // on FILES/sec instead — for a pure delete always, and for a mirror during
    // its purge phase. Everything else stays bytes.
    const bool itemsRate = ui->deleteMode || phaseDel;
    ui->itemsRateNow = itemsRate;
    const unsigned long long rateUnit = itemsRate ? dFiles : realDone;
    const unsigned long long rateTotal = itemsRate ? tFiles : totalReal;

    ui->hist.emplace_back(now, rateUnit);
    while (ui->hist.size() > 2 && (now - ui->hist.front().first) > 3000)
        ui->hist.pop_front();

    // Overall average: everything done so far divided by the time it took.
    double avgRate = (elapsed > 0.05) ? (double)rateUnit / elapsed : 0.0;

    double rate = 0.0;
    if (ui->done) {
        rate = avgRate; // final: overall average
    } else {
        const auto& oldest = ui->hist.front();
        double span = (now - oldest.first) / 1000.0;
        if (span >= 0.5 && rateUnit >= oldest.second)
            rate = (double)(rateUnit - oldest.second) / span;
        else
            rate = avgRate; // not enough history yet
    }

    // ETA uses the overall average, NOT the sliding window: the window reacts to
    // every burst and makes the estimate jump around uselessly. The average so
    // far also already accounts for enumeration overhead, which the rest incurs
    // too. Skipped volume is excluded — it costs microseconds, not time.
    double eta = avgRate > 1 ? (double)(rateTotal - rateUnit) / avgRate : -1;

    // total == 0 means nothing had to be copied (all skipped) — that is
    // complete, not 0%. Likewise the band must read full once finished, even if
    // the IO counters land a hair under the scanned total.
    int pct = total ? (int)((done * 100) / total) : 100;
    if (ui->done) pct = 100;
    ui->pct = pct;
    ui->rateNow = rate;

    // Colored taskbar-button progress: visible even when the dialog is
    // minimized or covered. Error state (red) is applied in WM_APP_DONE.
    if (ui->taskbar && !ui->done) {
        ui->taskbar->SetProgressState(ui->hwnd, TBPF_NORMAL);
        ui->taskbar->SetProgressValue(ui->hwnd, done, total ? total : 1);
    }

    // Feed the chart at its own cadence; the 100 ms UI tick would be too
    // noisy. Completion flushes unconditionally: a transfer that finishes
    // within the first interval (e.g. everything already at the destination)
    // would otherwise never write a single bucket and the band would carry no
    // skip marking at all. The curve's height is bytes/sec normally, files/sec
    // during a delete.
    if (now - ui->lastSampleTick >= CHART_SAMPLE_MS || ui->done) {
        double dt = (now - ui->lastSampleTick) / 1000.0;
        if (dt < 0.001) dt = 0.001;
        unsigned long long dReal =
            (realDone >= ui->lastSampleReal) ? realDone - ui->lastSampleReal : 0;
        unsigned long long dSkip =
            (skipDone >= ui->lastSampleSkip) ? skipDone - ui->lastSampleSkip : 0;
        unsigned long long dItems =
            (dFiles >= ui->lastSampleFiles) ? dFiles - ui->lastSampleFiles : 0;
        double curveVal = itemsRate ? (double)dItems / dt : (double)dReal / dt;
        RecordSample(ui, pct, curveVal, itemsRate ? dItems : dReal, dSkip);
        ui->lastSampleTick = now;
        ui->lastSampleReal = realDone;
        ui->lastSampleSkip = skipDone;
        ui->lastSampleFiles = dFiles;
    }
    InvalidateRect(ui->chart, nullptr, FALSE);

    if (ui->done) {
        wchar_t stats[256];
        if (itemsRate)
            StringCchPrintfW(stats, 256, loc::T(loc::S::StatsDoneItems), pct,
                             tFiles, loc::NounFile(tFiles),
                             (unsigned long long)rate);
        else
            StringCchPrintfW(stats, 256, loc::T(loc::S::StatsDone), pct,
                             HumanBytes(total).c_str(),
                             HumanBytes((unsigned long long)rate).c_str(), tFiles,
                             loc::NounFile(tFiles));
        SetWindowTextW(ui->lblName, stats);
        SetWindowTextW(ui->lblEta, L"");
        SetWindowTextW(ui->lblRemaining, L"");
        return;
    }

    if (phaseDel) {
        // Mirror delete phase: the copy volume is through (band sits at 100%);
        // deleting is metadata work with no meaningful byte rate or ETA.
        SetWindowTextW(ui->lblTitle, loc::T(loc::S::HeadDeleting));
        SetWindowTextW(ui->lblEta, L"");
        SetWindowTextW(ui->lblRemaining, L"");
    } else {
        wchar_t head[64];
        StringCchPrintfW(head, 64, loc::T(loc::S::HeadPercent), pct);
        SetWindowTextW(ui->lblTitle, head);
    }

    // Name: current file, path-compacted to fit the label.
    wchar_t compact[MAX_PATH];
    StringCchCopyW(compact, MAX_PATH, cur.c_str());
    HDC dc = GetDC(ui->lblName);
    HGDIOBJ old = SelectObject(dc, ui->font);
    RECT rc; GetClientRect(ui->lblName, &rc);
    PathCompactPathW(dc, compact, rc.right - rc.left - 60);
    SelectObject(dc, old);
    ReleaseDC(ui->lblName, dc);
    wchar_t line[MAX_PATH + 32];
    StringCchPrintfW(line, MAX_PATH + 32, loc::T(loc::S::LineName), compact);
    SetWindowTextW(ui->lblName, line);

    if (!phaseDel) {
        wchar_t etaLine[96];
        StringCchPrintfW(etaLine, 96, loc::T(loc::S::LineEta),
                         eta < 0 ? loc::T(loc::S::EtaCalculating)
                                 : FormatEta(eta).c_str());
        SetWindowTextW(ui->lblEta, etaLine);

        unsigned long long restFiles = (tFiles > dFiles) ? tFiles - dFiles : 0;
        wchar_t rem[128];
        StringCchPrintfW(rem, 128, loc::T(loc::S::LineRemaining), restFiles,
                         loc::NounFile(restFiles), HumanBytes(total - done).c_str());
        SetWindowTextW(ui->lblRemaining, rem);
    }
}

// Build the end-of-transfer report: what was skipped and why, then any errors.
// Reveal it and grow the window to fit.
void ShowReport(HWND hwnd, UiState* ui, bool failed) {
    std::wstring text;
    const SkipInfo& k = ui->skipped;
    wchar_t line[512];

    if (k.identicalFiles) {
        StringCchPrintfW(line, 512, loc::T(loc::S::SkipIdentical),
                         k.identicalFiles, loc::NounFile(k.identicalFiles),
                         HumanBytes(k.identicalBytes).c_str());
        text += line;
    }
    if (k.policyFiles) {
        const wchar_t* why = loc::T(ui->policy == Conflict::Skip
                                        ? loc::S::WhySkipExisting
                                        : loc::S::WhyOnlyNewer);
        StringCchPrintfW(line, 512, loc::T(loc::S::SkipPolicy), k.policyFiles,
                         loc::NounFile(k.policyFiles),
                         HumanBytes(k.policyBytes).c_str(), why);
        text += line;
    }

    EnterCriticalSection(&ui->sh->cs);
    size_t nErr = ui->sh->errors.size();
    if (nErr) {
        if (!text.empty()) text += L"\r\n";
        text += loc::T(loc::S::ErrorsLabel);
        for (auto& e : ui->sh->errors) { text += e; text += L"\r\n"; }
    } else if (failed) {
        if (!text.empty()) text += L"\r\n";
        text += loc::T(loc::S::NoErrorLines);
    }
    LeaveCriticalSection(&ui->sh->cs);

    SetWindowTextW(ui->edErrors, text.c_str());

    // Summary line above the list.
    wchar_t hdr[200];
    if (nErr) {
        StringCchPrintfW(hdr, 200, loc::T(loc::S::HdrErrors),
                         (unsigned long long)nErr, loc::NounError(nErr));
    } else {
        unsigned long long n = k.identicalFiles + k.policyFiles;
        StringCchPrintfW(hdr, 200, loc::T(loc::S::HdrSkipped), n,
                         loc::NounFile(n));
    }
    SetWindowTextW(ui->lblName, hdr);

    // Grow the client area, then place the box and move the Close button below
    // it (never overlapping).
    SetClientSize(hwnd, UI_W, UI_CH_REPORT);
    SetWindowPos(ui->edErrors, nullptr, UI_MARGIN, UI_BOX_Y,
                 UI_W - 2 * UI_MARGIN, UI_BOX_H, SWP_NOZORDER);
    ShowWindow(ui->edErrors, SW_SHOW);
    SetWindowPos(ui->btnCancel, nullptr, UI_W - UI_MARGIN - UI_BTN_W,
                 UI_BTN_Y_REPORT, UI_BTN_W, UI_BTN_H, SWP_NOZORDER);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UiState* ui = (UiState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    // Win32 controls do not follow the dark setting by themselves; the static
    // and edit backgrounds have to be painted here or they stay white.
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, theme::C().text);
        SetBkColor(dc, theme::C().bg);
        return (LRESULT)theme::BgBrush();
    }
    case WM_DRAWITEM:
        if (ui) theme::DrawButton((const DRAWITEMSTRUCT*)lp, ui->font);
        return TRUE;

    case WM_TIMER:
        if (wp == 2) {              // auto-close timer, armed on success
            DestroyWindow(hwnd);
            return 0;
        }
        if (ui && !ui->done) UpdateUi(ui); // id 1: periodic refresh
        return 0;

    case WM_COMMAND:
        if (ui && LOWORD(wp) == IDCANCEL) {
            if (ui->done) { DestroyWindow(hwnd); return 0; }
            // Request cancel: kill the running robocopy.
            InterlockedExchange(&ui->sh->cancel, 1);
            EnterCriticalSection(&ui->sh->cs);
            if (ui->sh->hCurrentProc) TerminateProcess(ui->sh->hCurrentProc, 1);
            LeaveCriticalSection(&ui->sh->cs);
            EnableWindow(ui->btnCancel, FALSE);
            SetWindowTextW(ui->lblTitle, loc::T(loc::S::TitleCancelling));
        }
        return 0;

    case WM_APP_DONE:
        if (ui) {
            ui->done = true;
            UpdateUi(ui); // forces the progress band to full
            int code = ui->sh->exitCode;
            bool cancelled = InterlockedCompareExchange(&ui->sh->cancel, 0, 0) != 0;
            EnterCriticalSection(&ui->sh->cs);
            bool hasErrors = !ui->sh->errors.empty();
            LeaveCriticalSection(&ui->sh->cs);

            if (cancelled) {
                SetWindowTextW(ui->lblTitle, loc::T(loc::S::TitleCancelled));
                SetWindowTextW(ui->btnCancel, loc::T(loc::S::BtnClose));
                EnableWindow(ui->btnCancel, TRUE);
                if (hasErrors) ShowReport(hwnd, ui, false);
                SetWindowTextW(ui->lblEta, L"");
                SetWindowTextW(ui->lblRemaining, L"");
            } else if (code >= 8 || hasErrors) {
                // code 8 = some files failed but copy continued; still surface it.
                if (ui->taskbar)
                    ui->taskbar->SetProgressState(ui->hwnd, TBPF_ERROR); // red
                SetWindowTextW(ui->lblTitle, loc::T(loc::S::TitleErrors));
                SetWindowTextW(ui->lblEta, L"");
                SetWindowTextW(ui->lblRemaining, L"");
                SetWindowTextW(ui->btnCancel, loc::T(loc::S::BtnClose));
                ShowReport(hwnd, ui, true);
            } else if (ui->skipped.any()) {
                // Nothing failed, but files were skipped — say so and stay open
                // so the user actually sees it (same as the error case).
                unsigned long long n =
                    ui->skipped.identicalFiles + ui->skipped.policyFiles;
                wchar_t t[128];
                StringCchPrintfW(t, 128, loc::T(loc::S::TitleDoneSkipped), n,
                                 loc::NounFile(n));
                SetWindowTextW(ui->lblTitle, t);
                SetWindowTextW(ui->btnCancel, loc::T(loc::S::BtnClose));
                EnableWindow(ui->btnCancel, TRUE);
                ShowReport(hwnd, ui, false);
            } else {
                // Nothing skipped, nothing failed: brief pause then auto-close.
                SetWindowTextW(ui->lblTitle, loc::T(loc::S::TitleDone));
                KillTimer(hwnd, 1);
                SetTimer(hwnd, 2, 700, nullptr);
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Ask the always-running tray agent (if present) to pop a "done" balloon on
// its persistent tray icon. Cross-process WM_COPYDATA; if the agent isn't
// running the balloon is simply skipped — the runner never grows a tray icon
// of its own (it exits right after, which would kill the balloon anyway).
void SendDoneBalloon(const std::wstring& body) {
    HWND agent = FindWindowW(L"AngelCopyAgentWnd", nullptr);
    if (!agent) return;
    COPYDATASTRUCT cds{};
    cds.dwData = 1; // "show done balloon"
    cds.cbData = (DWORD)((body.size() + 1) * sizeof(wchar_t));
    cds.lpData = (PVOID)body.c_str();
    SendMessageTimeoutW(agent, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG,
                        1000, nullptr);
}

// Show the dialog and run `worker` on a background thread until it finishes or
// the user cancels. Shared by the copy and delete paths.
// `skipSet`/`dstPrefixes` may be null/empty (the delete path has no robocopy
// output to classify).
int RunUI(const std::wstring& caption, const std::wstring& heading,
          unsigned long long expectedBytes, unsigned long long expectedFiles,
          Conflict policy, SkipInfo skipped,
          const std::unordered_set<std::wstring>* skipSet,
          std::vector<std::wstring> dstPrefixes, Worker worker,
          bool deleteMode = false) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    Shared sh;
    // The bar spans everything robocopy will walk over — copied AND skipped —
    // so a half-existing destination doesn't race through a tiny remainder.
    // Speed/ETA stay scaled to the real (copied) volume.
    sh.totalBytes = expectedBytes + skipped.identicalBytes + skipped.policyBytes;
    sh.totalRealBytes = expectedBytes;
    sh.totalFiles = expectedFiles + skipped.identicalFiles + skipped.policyFiles;
    sh.skipSet = skipSet;
    sh.dstPrefixes = std::move(dstPrefixes);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyProgress";
    RegisterClassW(&wc);

    WNDCLASSW cc{};
    cc.lpfnWndProc = ChartProc;
    cc.hInstance = hInst;
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc.hbrBackground = nullptr; // PaintChart fills everything
    cc.lpszClassName = L"AngelCopyChart";
    RegisterClassW(&cc);

    // Derive the window size from the desired client size.
    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, UI_W, UI_CH_NORMAL};
    AdjustWindowRectEx(&rc, kStyle, FALSE, WS_EX_TOPMOST);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, caption.c_str(),
                                kStyle, sx, sy, W, H, nullptr, nullptr, hInst,
                                nullptr);
    if (!hwnd) return 16;

    UiState ui{};
    ui.sh = &sh;
    ui.hwnd = hwnd;
    ui.deleteMode = deleteMode;
    ui.skipped = skipped;
    ui.policy = policy;

    // Colored taskbar-button progress (optional; ignored if it can't be had).
    // COM was not otherwise needed in the runner UI thread — init it here.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&ui.taskbar)))) {
        if (FAILED(ui.taskbar->HrInit())) {
            ui.taskbar->Release();
            ui.taskbar = nullptr;
        }
    }
    ui.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        ui.fontBold = CreateFontIndirectW(&bold);
    }
    if (!ui.fontBold) ui.fontBold = ui.font;

    ui.lblTitle = CreateWindowW(L"STATIC", heading.c_str(),
        WS_CHILD | WS_VISIBLE, UI_MARGIN, 12, UI_W - 2 * UI_MARGIN, 20, hwnd,
        nullptr, hInst, nullptr);
    ui.chart = CreateWindowExW(0, L"AngelCopyChart", nullptr,
        WS_CHILD | WS_VISIBLE, UI_MARGIN, UI_CHART_Y, UI_W - 2 * UI_MARGIN,
        UI_CHART_H, hwnd, nullptr, hInst, nullptr);
    ui.lblName = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, UI_MARGIN, UI_LINE1_Y,
        UI_W - 2 * UI_MARGIN, 18, hwnd, nullptr, hInst, nullptr);
    ui.lblEta = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        UI_MARGIN, UI_LINE2_Y, UI_W - 2 * UI_MARGIN, 18, hwnd, nullptr, hInst,
        nullptr);
    ui.lblRemaining = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        UI_MARGIN, UI_LINE3_Y, UI_W - 2 * UI_MARGIN, 18, hwnd, nullptr, hInst,
        nullptr);
    ui.btnCancel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCancel),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false),
        UI_W - UI_MARGIN - UI_BTN_W, UI_BTN_Y, UI_BTN_W, UI_BTN_H, hwnd,
        (HMENU)IDCANCEL, hInst, nullptr);
    // Report box — hidden until there is something to report, then revealed and
    // the window grows to show it (see ShowReport).
    ui.edErrors = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        UI_MARGIN, UI_BOX_Y, UI_W - 2 * UI_MARGIN, UI_BOX_H, hwnd, nullptr,
        hInst, nullptr);

    SetChildFont(ui.lblTitle, ui.fontBold);
    SetChildFont(ui.lblName, ui.font);
    SetChildFont(ui.lblEta, ui.font);
    SetChildFont(ui.lblRemaining, ui.font);
    SetChildFont(ui.btnCancel, ui.font);
    SetChildFont(ui.edErrors, ui.font);

    ui.startTick = GetTickCount64();
    ui.lastSampleTick = ui.startTick; // sample deltas start at 0 bytes, now
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&ui);
    SetWindowLongPtrW(ui.chart, GWLP_USERDATA, (LONG_PTR)&ui);

    theme::ApplyToWindow(hwnd);
    theme::ApplyToControl(ui.btnCancel);
    theme::ApplyToControl(ui.edErrors);

    EngineArgs ea{&sh, hwnd, std::move(worker)};
    HANDLE th = CreateThread(nullptr, 0, EngineThread, &ea, 0, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 100, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }

    int code = sh.exitCode;
    const bool cancelled = InterlockedCompareExchange(&sh.cancel, 0, 0) != 0;
    if (cancelled) code = -1;

    // "Done" balloon via the agent — only for a clean run that took long
    // enough to be worth a notification (quick copies don't earn a popup).
    double elapsed = (GetTickCount64() - ui.startTick) / 1000.0;
    if (!cancelled && code < 8 && elapsed >= 3.0) {
        unsigned long long vol = sh.totalRealBytes ? sh.totalRealBytes
                                                   : sh.totalBytes;
        wchar_t body[160];
        StringCchPrintfW(body, 160, loc::T(loc::S::NotifyDoneBody),
                         HumanBytes(vol).c_str(), FormatEta(elapsed).c_str());
        SendDoneBalloon(body);
    }

    if (ui.taskbar) {
        ui.taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
        ui.taskbar->Release();
    }
    CoUninitialize();
    if (ui.fontBold && ui.fontBold != ui.font) DeleteObject(ui.fontBold);
    if (ui.font && ui.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(ui.font);
    return code;
}

} // namespace

// ---- "Preparing" window (pre-transfer scan feedback) -----------------------

namespace {

struct ScanUi {
    ScanProgress* prog = nullptr;
    HWND lblHead = nullptr, lblCount = nullptr, btnCancel = nullptr;
    HFONT font = nullptr, fontBold = nullptr;
    bool done = false;
    bool shown = false;
};

constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
constexpr int SCAN_W = 380, SCAN_H = 116;

LRESULT CALLBACK ScanWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ScanUi* ui = (ScanUi*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, theme::C().text);
        SetBkColor(dc, theme::C().bg);
        return (LRESULT)theme::BgBrush();
    }
    case WM_DRAWITEM:
        if (ui) theme::DrawButton((const DRAWITEMSTRUCT*)lp, ui->font);
        return TRUE;
    case WM_TIMER:
        if (ui) {
            if (wp == 3 && !ui->done && !ui->shown) {
                // The reveal timer: only a scan still running after ~300 ms
                // gets a window — small transfers never flash one.
                ui->shown = true;
                ShowWindow(hwnd, SW_SHOW);
            }
            if (wp == 1 && ui->shown && !ui->done) {
                wchar_t line[128];
                unsigned long long n = ui->prog->files.load(std::memory_order_relaxed);
                StringCchPrintfW(line, 128, loc::T(loc::S::ScanFound), n,
                                 loc::NounFile(n));
                SetWindowTextW(ui->lblCount, line);
            }
        }
        return 0;
    case WM_COMMAND:
        if (ui && LOWORD(wp) == IDCANCEL && !ui->done) {
            ui->prog->cancel.store(1);
            SetWindowTextW(ui->lblHead, loc::T(loc::S::TitleCancelling));
            EnableWindow(ui->btnCancel, FALSE);
        }
        return 0;
    case WM_CLOSE:
        // Treat closing like Cancel: stop the scan, then the worker's done
        // message tears the window down.
        if (ui && !ui->done) {
            ui->prog->cancel.store(1);
            SetWindowTextW(ui->lblHead, loc::T(loc::S::TitleCancelling));
            EnableWindow(ui->btnCancel, FALSE);
        }
        return 0;
    case WM_APP_SCAN_DONE:
        if (ui) ui->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 3);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct ScanThreadArgs {
    const std::function<void()>* work;
    HWND hwnd;
};

DWORD WINAPI ScanThread(LPVOID p) {
    ScanThreadArgs* a = (ScanThreadArgs*)p;
    (*a->work)();
    PostMessageW(a->hwnd, WM_APP_SCAN_DONE, 0, 0);
    return 0;
}

} // namespace

bool RunScanWithUI(ScanProgress& prog, const std::function<void()>& work) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ScanWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyScan";
    RegisterClassW(&wc);

    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, SCAN_W, SCAN_H};
    AdjustWindowRectEx(&rc, kStyle, FALSE, WS_EX_TOPMOST);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                loc::T(loc::S::CapPreparing), kStyle, sx, sy, W,
                                H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { work(); return prog.cancel.load() == 0; } // no window: degrade

    ScanUi ui{};
    ui.prog = &prog;
    ui.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        ui.fontBold = CreateFontIndirectW(&bold);
    }
    if (!ui.fontBold) ui.fontBold = ui.font;

    ui.lblHead = CreateWindowW(L"STATIC", loc::T(loc::S::HeadPreparing),
        WS_CHILD | WS_VISIBLE, UI_MARGIN, 14, SCAN_W - 2 * UI_MARGIN, 20, hwnd,
        nullptr, hInst, nullptr);
    ui.lblCount = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE, UI_MARGIN, 40, SCAN_W - 2 * UI_MARGIN, 18, hwnd,
        nullptr, hInst, nullptr);
    ui.btnCancel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCancel),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false),
        SCAN_W - UI_MARGIN - UI_BTN_W, SCAN_H - UI_BTN_H - 14, UI_BTN_W,
        UI_BTN_H, hwnd, (HMENU)IDCANCEL, hInst, nullptr);
    SetChildFont(ui.lblHead, ui.fontBold);
    SetChildFont(ui.lblCount, ui.font);
    SetChildFont(ui.btnCancel, ui.font);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&ui);
    theme::ApplyToWindow(hwnd);
    theme::ApplyToControl(ui.btnCancel);

    SetTimer(hwnd, 1, 100, nullptr); // counter refresh
    SetTimer(hwnd, 3, 300, nullptr); // reveal delay (anti-flicker)

    ScanThreadArgs args{&work, hwnd};
    HANDLE th = CreateThread(nullptr, 0, ScanThread, &args, 0, nullptr);
    if (!th) { // cannot thread: run inline, window stays hidden
        work();
        DestroyWindow(hwnd);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }

    if (ui.fontBold && ui.fontBold != ui.font) DeleteObject(ui.fontBold);
    if (ui.font && ui.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(ui.font);
    return prog.cancel.load() == 0;
}

int RunJobsWithUI(Operation op, const std::wstring& destLabel,
                  const std::vector<RoboJob>& jobs,
                  unsigned long long expectedBytes,
                  unsigned long long expectedFiles, Conflict policy,
                  SkipInfo skipped,
                  const std::unordered_set<std::wstring>& skipSet) {
    (void)destLabel;
    // Destination roots, lowercased, to filter robocopy's "extra file" lines
    // (they carry destination paths; copy/skip lines carry source paths).
    std::vector<std::wstring> dstPrefixes;
    for (const RoboJob& job : jobs) {
        std::wstring p = LowerCopy(job.dstDir);
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        dstPrefixes.push_back(std::move(p));
    }
    const bool move = (op == Operation::Move);
    return RunUI(loc::T(move ? loc::S::CapMoving : loc::S::CapCopying),
                 loc::T(move ? loc::S::HeadMoving : loc::S::HeadCopying),
                 expectedBytes, expectedFiles, policy, skipped, &skipSet,
                 std::move(dstPrefixes),
                 [op, &jobs, policy](Shared& sh) {
                     return RunCopyJobs(sh, op, jobs, policy);
                 });
}

int RunDeleteWithUI(const std::vector<std::wstring>& targets,
                    unsigned long long expectedBytes,
                    unsigned long long expectedFiles) {
    return RunUI(loc::T(loc::S::CapDeleting), loc::T(loc::S::HeadDeleting),
                 expectedBytes, expectedFiles, Conflict::Replace, SkipInfo{},
                 nullptr, {},
                 [&targets](Shared& sh) { return RunDelete(sh, targets); },
                 /*deleteMode=*/true);
}

int RunSyncWithUI(const std::vector<RoboJob>& jobs,
                  unsigned long long expectedBytes,
                  unsigned long long expectedFiles, SkipInfo skipped,
                  const std::unordered_set<std::wstring>& skipSet,
                  const std::vector<std::wstring>& extraTargets) {
    std::vector<std::wstring> dstPrefixes;
    for (const RoboJob& job : jobs) {
        std::wstring p = LowerCopy(job.dstDir);
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        dstPrefixes.push_back(std::move(p));
    }
    // Copy first, then purge: never delete anything while the copy that might
    // still fail is running. A cancel between the phases leaves a superset of
    // the source at the destination — safe.
    return RunUI(loc::T(loc::S::CapSyncing), loc::T(loc::S::HeadSyncing),
                 expectedBytes, expectedFiles, Conflict::Replace, skipped,
                 &skipSet, std::move(dstPrefixes),
                 [&jobs, &extraTargets](Shared& sh) {
                     int rc = RunCopyJobs(sh, Operation::Copy, jobs,
                                          Conflict::Replace);
                     if (InterlockedCompareExchange(&sh.cancel, 0, 0)) return rc;
                     if (rc >= 8) return rc; // copy failed: do NOT purge
                     EnterCriticalSection(&sh.cs);
                     sh.phaseDelete = true;
                     LeaveCriticalSection(&sh.cs);
                     int rd = RunDelete(sh, extraTargets);
                     return rd > rc ? rd : rc;
                 });
}

} // namespace angelcopy
