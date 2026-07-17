#include "ConfirmUI.h"
#include "../shared/Localize.h"
#include "../shared/Theme.h"

#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")

namespace angelcopy {

namespace {

constexpr int ID_DELETE = 1101;
constexpr int CW = 520;
constexpr int CH = 300;

struct State {
    bool confirmed = false;
    HFONT font = nullptr, fontBold = nullptr;
};

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = (State*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, theme::C().text);
        SetBkColor((HDC)wp, theme::C().bg);
        return (LRESULT)theme::BgBrush();
    case WM_DRAWITEM:
        if (st) theme::DrawButton((const DRAWITEMSTRUCT*)lp, st->font);
        return TRUE;
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == ID_DELETE) { st->confirmed = true; DestroyWindow(hwnd); return 0; }
        if (LOWORD(wp) == IDCANCEL) { st->confirmed = false; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd); // stays unconfirmed
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SetFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

std::wstring HumanBytes(unsigned long long b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t out[64];
    StringCchPrintfW(out, 64, (v < 10 && i > 0) ? L"%.1f %s" : L"%.0f %s", v, u[i]);
    return out;
}

} // namespace

// Mirror confirmation. Shares the window class/pattern with the delete prompt:
// owner-drawn buttons in dark mode, Cancel default, closing never confirms.
bool AskSyncConfirm(unsigned long long copyFiles, unsigned long long copyBytes,
                    const DeleteScan& del) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyConfirmSync";
    RegisterClassW(&wc);

    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, CW, CH};
    AdjustWindowRectEx(&rc, kStyle, FALSE, WS_EX_TOPMOST);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                loc::T(loc::S::SyncCaption), kStyle,
                                sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return false;

    State st;
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

    unsigned long long delItems = del.files + del.dirs;
    wchar_t copyLine[200], delLine[200];
    StringCchPrintfW(copyLine, 200,
                     loc::T(copyFiles == 1 ? loc::S::SyncCopyOne
                                           : loc::S::SyncCopyMany),
                     copyFiles, HumanBytes(copyBytes).c_str());
    if (delItems) {
        StringCchPrintfW(delLine, 200,
                         loc::T(delItems == 1 ? loc::S::SyncDelOne
                                              : loc::S::SyncDelMany),
                         delItems, HumanBytes(del.bytes).c_str());
    } else {
        StringCchCopyW(delLine, 200, loc::T(loc::S::SyncDelNone));
    }

    HWND lblCopy = CreateWindowW(L"STATIC", copyLine, WS_CHILD | WS_VISIBLE,
                                 16, 14, CW - 32, 20, hwnd, nullptr, hInst, nullptr);
    HWND lblDel = CreateWindowW(L"STATIC", delLine, WS_CHILD | WS_VISIBLE,
                                16, 38, CW - 32, 20, hwnd, nullptr, hInst, nullptr);

    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                    LBS_NOSEL | LBS_NOINTEGRALHEIGHT,
                                16, 64, CW - 32, 146, hwnd, nullptr, hInst, nullptr);
    for (const auto& s : del.sample)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());

    HWND lblWarn = CreateWindowW(L"STATIC", loc::T(loc::S::SyncWarn),
                                 WS_CHILD | WS_VISIBLE, 16, 218, CW - 32, 20,
                                 hwnd, nullptr, hInst, nullptr);

    const int by = 248, bh = 30;
    HWND bSync = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnSync),
                               WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false),
                               16, by, 170, bh, hwnd, (HMENU)(INT_PTR)ID_DELETE,
                               hInst, nullptr);
    // Cancel is the default button: Enter/Esc must never mirror.
    HWND bCancel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCancel),
                                 WS_CHILD | WS_VISIBLE | theme::ButtonStyle(true),
                                 CW - 116, by, 100, bh, hwnd,
                                 (HMENU)(INT_PTR)IDCANCEL, hInst, nullptr);

    SetFont(lblCopy, st.fontBold);
    SetFont(lblDel, st.fontBold);
    SetFont(lblWarn, st.font);
    SetFont(list, st.font);
    SetFont(bSync, st.font);
    SetFont(bCancel, st.font);

    theme::ApplyToWindow(hwnd);
    for (HWND b : {bSync, bCancel, list}) theme::ApplyToControl(b);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(bCancel);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (st.fontBold && st.fontBold != st.font) DeleteObject(st.fontBold);
    if (st.font && st.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(st.font);
    return st.confirmed;
}

bool AskDeleteConfirm(const DeleteScan& scan) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyConfirmDelete";
    RegisterClassW(&wc);

    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, CW, CH};
    AdjustWindowRectEx(&rc, kStyle, FALSE, WS_EX_TOPMOST);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                loc::T(loc::S::DeleteCaption), kStyle,
                                sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return false;

    State st;
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

    wchar_t head[200];
    StringCchPrintfW(head, 200, loc::T(loc::S::DeleteHead), scan.files,
                     loc::NounFile(scan.files), HumanBytes(scan.bytes).c_str(),
                     scan.dirs, loc::NounFoldersIn(scan.dirs));
    HWND lblHead = CreateWindowW(L"STATIC", head, WS_CHILD | WS_VISIBLE, 16, 14,
                                 CW - 32, 20, hwnd, nullptr, hInst, nullptr);
    HWND lblWarn = CreateWindowW(L"STATIC", loc::T(loc::S::DeleteWarn),
                                 WS_CHILD | WS_VISIBLE, 16, 38, CW - 32, 20, hwnd,
                                 nullptr, hInst, nullptr);

    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                    LBS_NOSEL | LBS_NOINTEGRALHEIGHT,
                                16, 64, CW - 32, 170, hwnd, nullptr, hInst, nullptr);
    for (const auto& s : scan.sample)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());

    const int by = 248, bh = 30;
    HWND bDel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnDeletePerm),
                              WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false), 16, by, 170,
                              bh, hwnd, (HMENU)(INT_PTR)ID_DELETE, hInst, nullptr);
    // Cancel is the default button: Enter/Esc must never delete.
    HWND bCancel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCancel),
                                 WS_CHILD | WS_VISIBLE | theme::ButtonStyle(true),
                                 CW - 116, by, 100, bh, hwnd,
                                 (HMENU)(INT_PTR)IDCANCEL, hInst, nullptr);

    SetFont(lblHead, st.fontBold);
    SetFont(lblWarn, st.font);
    SetFont(list, st.font);
    SetFont(bDel, st.font);
    SetFont(bCancel, st.font);

    theme::ApplyToWindow(hwnd);
    for (HWND b : {bDel, bCancel, list}) theme::ApplyToControl(b);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(bCancel);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (st.fontBold && st.fontBold != st.font) DeleteObject(st.fontBold);
    if (st.font && st.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(st.font);
    return st.confirmed;
}

} // namespace angelcopy
