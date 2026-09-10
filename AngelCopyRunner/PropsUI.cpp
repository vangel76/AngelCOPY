#include "PropsUI.h"
#include "Delete.h"
#include "../shared/Localize.h"
#include "../shared/Theme.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <atomic>
#include <thread>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Ole32.lib")

namespace angelcopy {

namespace {

constexpr int ID_WINPROPS = 1201;
constexpr int ID_CALCALLOC = 1202;
constexpr UINT_PTR TIMER_REFRESH = 1; // branch on the id (WM_TIMER gotcha)
constexpr int CW = 460;
constexpr int CH = 284;

std::wstring ExtP(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring BaseNameOf(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    size_t i = s.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? s : s.substr(i + 1);
}

std::wstring HumanBytes(unsigned long long b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t out[64];
    StringCchPrintfW(out, 64, (v < 10 && i > 0) ? L"%.1f %s" : L"%.0f %s", v, u[i]);
    return out;
}

// Locale-formatted local date + time for a UTC FILETIME.
std::wstring FormatWhen(const FILETIME& ftUtc) {
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ftUtc, &utc)) return L"\x2014";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t d[64]{}, t[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr,
                    d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr,
                    t, 64);
    return std::wstring(d) + L" " + t;
}

std::wstring AttrWords(DWORD a) {
    std::wstring s;
    auto add = [&](loc::S id) {
        if (!s.empty()) s += L", ";
        s += loc::T(id);
    };
    if (a & FILE_ATTRIBUTE_DIRECTORY)  add(loc::S::AttrDirectory);
    if (a & FILE_ATTRIBUTE_READONLY)   add(loc::S::AttrReadonly);
    if (a & FILE_ATTRIBUTE_HIDDEN)     add(loc::S::AttrHidden);
    if (a & FILE_ATTRIBUTE_SYSTEM)     add(loc::S::AttrSystem);
    if (a & FILE_ATTRIBUTE_ARCHIVE)    add(loc::S::AttrArchive);
    if (a & FILE_ATTRIBUTE_COMPRESSED) add(loc::S::AttrCompressed);
    return s.empty() ? L"\x2014" : s;
}

// Open the NATIVE properties sheet — everything we deliberately don't rebuild
// (Security, Sharing, Previous Versions, Details, ...). The sheet runs on its
// own thread INSIDE this process; ShowProps keeps the process alive until it
// closes (see the wait loop there).
void OpenNativeProps(const std::vector<std::wstring>& targets) {
    if (targets.size() == 1) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = targets[0].c_str();
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
        return;
    }
    // Multi-selection: one combined sheet, exactly like Explorer's.
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : targets) {
        PIDLIST_ABSOLUTE pi = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pi, 0, nullptr)) && pi)
            pidls.push_back(pi);
    }
    if (!pidls.empty()) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(
                (UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(),
                &arr)) &&
            arr) {
            IDataObject* dobj = nullptr;
            if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject,
                                             IID_PPV_ARGS(&dobj))) &&
                dobj) {
                SHMultiFileProperties(dobj, 0);
                dobj->Release();
            }
            arr->Release();
        }
    }
    for (auto pi : pidls) CoTaskMemFree(pi);
}

struct PState {
    std::vector<std::wstring> targets;
    ScanProgress countProg, allocProg;
    DeleteScan result;                 // exact totals once countDone
    std::atomic<bool> countDone{false};
    std::atomic<bool> allocRunning{false}, allocDone{false};
    unsigned long long allocTotal = 0; // valid once allocDone
    std::thread countThread, allocThread;
    bool nativeOpened = false;

    HWND lblContent = nullptr, lblSize = nullptr, lblAlloc = nullptr;
    HWND btnAlloc = nullptr;
    HFONT font = nullptr, fontBold = nullptr;
};

void RefreshCounts(PState* st) {
    // Live values from the scan's atomics until it finishes, then the exact
    // merged result (the atomics and the result agree; this just stops the
    // trailing ellipsis).
    const bool done = st->countDone.load(std::memory_order_acquire);
    unsigned long long files =
        done ? st->result.files : st->countProg.files.load(std::memory_order_relaxed);
    unsigned long long dirs =
        done ? st->result.dirs : st->countProg.dirs.load(std::memory_order_relaxed);
    unsigned long long bytes =
        done ? st->result.bytes : st->countProg.bytes.load(std::memory_order_relaxed);

    wchar_t line[256];
    StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineContent), files,
                     loc::NounFile(files), dirs, loc::NounFolders(dirs));
    if (!done) StringCchCatW(line, 256, L" \x2026");
    SetWindowTextW(st->lblContent, line);

    StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineSize),
                     HumanBytes(bytes).c_str(), bytes);
    if (!done) StringCchCatW(line, 256, L" \x2026");
    SetWindowTextW(st->lblSize, line);

    if (st->allocDone.load(std::memory_order_acquire)) {
        StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineAlloc),
                         HumanBytes(st->allocTotal).c_str());
        SetWindowTextW(st->lblAlloc, line);
    } else if (st->allocRunning.load(std::memory_order_relaxed)) {
        unsigned long long a =
            st->allocProg.bytes.load(std::memory_order_relaxed);
        StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineAlloc),
                         HumanBytes(a).c_str());
        StringCchCatW(line, 256, L" \x2026");
        SetWindowTextW(st->lblAlloc, line);
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PState* st = (PState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, theme::C().text);
        SetBkColor((HDC)wp, theme::C().bg);
        return (LRESULT)theme::BgBrush();
    case WM_DRAWITEM:
        if (st) theme::DrawButton((const DRAWITEMSTRUCT*)lp, st->font);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && st) { RefreshCounts(st); return 0; }
        break;
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == ID_WINPROPS) {
            st->nativeOpened = true;
            OpenNativeProps(st->targets);
            return 0;
        }
        if (LOWORD(wp) == ID_CALCALLOC) {
            if (!st->allocRunning.exchange(true)) {
                EnableWindow(st->btnAlloc, FALSE);
                st->allocThread = std::thread([st] {
                    st->allocTotal = ScanAllocated(st->targets, &st->allocProg);
                    st->allocDone.store(true, std::memory_order_release);
                });
            }
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SetFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

} // namespace

void ShowProps(const std::vector<std::wstring>& targets) {
    if (targets.empty()) return;
    // STA for the "Windows properties" button (verb invocation / data object).
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyProps";
    RegisterClassW(&wc);

    // NOT topmost: the native sheet the button opens must be able to cover us.
    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, CW, CH};
    AdjustWindowRectEx(&rc, kStyle, FALSE, 0);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, loc::T(loc::S::PropsCaption),
                                kStyle, sx, sy, W, H, nullptr, nullptr, hInst,
                                nullptr);
    if (!hwnd) {
        if (SUCCEEDED(comInit)) CoUninitialize();
        return;
    }

    PState st;
    st.targets = targets;
    st.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        st.font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW b = ncm.lfMessageFont;
        b.lfWeight = FW_SEMIBOLD;
        st.fontBold = CreateFontIndirectW(&b);
    }
    if (!st.fontBold) st.fontBold = st.font;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    // Header: name + path (single target), "N items selected" + parent (multi).
    const bool single = targets.size() == 1;
    std::wstring nameText, pathText;
    if (single) {
        nameText = BaseNameOf(targets[0]);
        pathText = targets[0];
    } else {
        wchar_t buf[64];
        StringCchPrintfW(buf, 64, loc::T(loc::S::PropsMultiName),
                         (unsigned long long)targets.size());
        nameText = buf;
        std::wstring parent = targets[0];
        size_t cut = parent.find_last_of(L'\\');
        pathText = (cut == std::wstring::npos) ? parent : parent.substr(0, cut);
    }

    HWND lblName = CreateWindowW(L"STATIC", nameText.c_str(),
                                 WS_CHILD | WS_VISIBLE, 16, 14, CW - 32, 20,
                                 hwnd, nullptr, hInst, nullptr);
    HWND lblPath = CreateWindowW(L"STATIC", pathText.c_str(),
                                 WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 16, 36,
                                 CW - 32, 18, hwnd, nullptr, hInst, nullptr);

    st.lblContent = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 68,
                                  CW - 32, 20, hwnd, nullptr, hInst, nullptr);
    st.lblSize = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 92,
                               CW - 32, 20, hwnd, nullptr, hInst, nullptr);

    wchar_t allocInit[64];
    StringCchPrintfW(allocInit, 64, loc::T(loc::S::PropsLineAlloc), L"\x2014");
    st.lblAlloc = CreateWindowW(L"STATIC", allocInit, WS_CHILD | WS_VISIBLE, 16,
                                116, CW - 32 - 166, 20, hwnd, nullptr, hInst,
                                nullptr);
    st.btnAlloc = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCalcAlloc),
                                WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false),
                                CW - 16 - 150, 111, 150, 26, hwnd,
                                (HMENU)(INT_PTR)ID_CALCALLOC, hInst, nullptr);

    // Timestamps + attributes: single target only (an aggregate would lie).
    HWND lblCreated = nullptr, lblModified = nullptr, lblAttrs = nullptr;
    if (single) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        // ExtP: a >MAX_PATH target must not read as "no data" (\\?\ gotcha).
        if (GetFileAttributesExW(ExtP(targets[0]).c_str(), GetFileExInfoStandard,
                                 &fa)) {
            wchar_t line[256];
            StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineCreated),
                             FormatWhen(fa.ftCreationTime).c_str());
            lblCreated = CreateWindowW(L"STATIC", line, WS_CHILD | WS_VISIBLE,
                                       16, 148, CW - 32, 20, hwnd, nullptr,
                                       hInst, nullptr);
            StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineModified),
                             FormatWhen(fa.ftLastWriteTime).c_str());
            lblModified = CreateWindowW(L"STATIC", line, WS_CHILD | WS_VISIBLE,
                                        16, 172, CW - 32, 20, hwnd, nullptr,
                                        hInst, nullptr);
            StringCchPrintfW(line, 256, loc::T(loc::S::PropsLineAttrs),
                             AttrWords(fa.dwFileAttributes).c_str());
            lblAttrs = CreateWindowW(L"STATIC", line, WS_CHILD | WS_VISIBLE, 16,
                                     196, CW - 32, 20, hwnd, nullptr, hInst,
                                     nullptr);
        }
    }

    const int by = 236, bh = 30;
    HWND btnWin = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnWinProps),
                                WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false),
                                16, by, 190, bh, hwnd,
                                (HMENU)(INT_PTR)ID_WINPROPS, hInst, nullptr);
    HWND btnClose = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnClose),
                                  WS_CHILD | WS_VISIBLE | theme::ButtonStyle(true),
                                  CW - 116, by, 100, bh, hwnd,
                                  (HMENU)(INT_PTR)IDCANCEL, hInst, nullptr);

    SetFont(lblName, st.fontBold);
    SetFont(lblPath, st.font);
    SetFont(st.lblContent, st.font);
    SetFont(st.lblSize, st.font);
    SetFont(st.lblAlloc, st.font);
    for (HWND w : {lblCreated, lblModified, lblAttrs})
        if (w) SetFont(w, st.font);
    SetFont(st.btnAlloc, st.font);
    SetFont(btnWin, st.font);
    SetFont(btnClose, st.font);

    theme::ApplyToWindow(hwnd);
    for (HWND b : {st.btnAlloc, btnWin, btnClose}) theme::ApplyToControl(b);

    // The counter starts immediately; the dialog is already visible while it
    // runs (that is the whole point vs. Explorer's dialog).
    st.countThread = std::thread([&st] {
        st.result = ScanDelete(st.targets, &st.countProg);
        st.countDone.store(true, std::memory_order_release);
    });

    RefreshCounts(&st);
    SetTimer(hwnd, TIMER_REFRESH, 100, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(btnClose);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    // Closing cancels any still-running walk; both exit promptly.
    st.countProg.cancel.store(1, std::memory_order_relaxed);
    st.allocProg.cancel.store(1, std::memory_order_relaxed);
    if (st.countThread.joinable()) st.countThread.join();
    if (st.allocThread.joinable()) st.allocThread.join();

    // The native properties sheet lives on a thread INSIDE this process —
    // exiting now would close it under the user's cursor. Wait until every
    // visible top-level window of this process is gone.
    if (st.nativeOpened) {
        for (;;) {
            struct Ctx { DWORD pid; bool any; } ctx{GetCurrentProcessId(), false};
            EnumWindows(
                [](HWND w, LPARAM p) -> BOOL {
                    Ctx* c = (Ctx*)p;
                    DWORD pid = 0;
                    GetWindowThreadProcessId(w, &pid);
                    if (pid == c->pid && IsWindowVisible(w)) {
                        c->any = true;
                        return FALSE;
                    }
                    return TRUE;
                },
                (LPARAM)&ctx);
            if (!ctx.any) break;
            Sleep(300);
        }
    }

    if (st.fontBold && st.fontBold != st.font) DeleteObject(st.fontBold);
    if (st.font && st.font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(st.font);
    if (SUCCEEDED(comInit)) CoUninitialize();
}

} // namespace angelcopy
