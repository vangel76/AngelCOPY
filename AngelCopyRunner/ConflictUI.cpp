#include "ConflictUI.h"
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

constexpr int ID_REPLACE = 1001;
constexpr int ID_SKIP    = 1002;
constexpr int ID_NEWER   = 1003;
constexpr int ID_CANCEL  = IDCANCEL;

// CLIENT dimensions; the window size is derived with AdjustWindowRectEx below.
// Layout: head 12, sub 34, list 58..238, buttons 250..280 and 288..318, margin.
constexpr int CW = 560;
constexpr int CH = 334;

struct DlgState {
    Conflict choice = Conflict::Replace;
    bool cancelled = true;   // closing the window == cancel
    bool decided = false;
    HFONT font = nullptr, fontBold = nullptr;
};

LRESULT CALLBACK ConflictProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* st = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
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
        switch (LOWORD(wp)) {
        case ID_REPLACE: st->choice = Conflict::Replace;        st->cancelled = false; st->decided = true; DestroyWindow(hwnd); return 0;
        case ID_SKIP:    st->choice = Conflict::Skip;           st->cancelled = false; st->decided = true; DestroyWindow(hwnd); return 0;
        case ID_NEWER:   st->choice = Conflict::ReplaceIfNewer; st->cancelled = false; st->decided = true; DestroyWindow(hwnd); return 0;
        case ID_CANCEL:  st->cancelled = true; st->decided = true; DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SetFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

} // namespace

Conflict AskConflict(Operation op, unsigned long long conflictCount,
                     const std::vector<std::wstring>& sample, bool& cancelled) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ConflictProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::BgBrush();
    wc.lpszClassName = L"AngelCopyConflict";
    RegisterClassW(&wc);

    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, CW, CH};
    AdjustWindowRectEx(&rc, kStyle, FALSE, WS_EX_TOPMOST);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, wc.lpszClassName, loc::T(loc::S::ConflictCaption), kStyle,
        sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { cancelled = false; return Conflict::Replace; }

    DlgState st;
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
    StringCchPrintfW(head, 200,
                     loc::T(conflictCount == 1 ? loc::S::ConflictHeadOne
                                               : loc::S::ConflictHeadMany),
                     conflictCount);
    HWND lblHead = CreateWindowW(L"STATIC", head, WS_CHILD | WS_VISIBLE,
                                 16, 12, CW - 32, 20, hwnd, nullptr, hInst, nullptr);
    HWND lblSub = CreateWindowW(
        L"STATIC",
        loc::T(op == Operation::Move ? loc::S::ConflictSubMove
                                     : loc::S::ConflictSubCopy),
        WS_CHILD | WS_VISIBLE, 16, 34, CW - 32, 18, hwnd, nullptr, hInst, nullptr);

    // List of the files that would be overwritten.
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                    LBS_NOSEL | LBS_NOINTEGRALHEIGHT,
                                16, 58, CW - 32, 180, hwnd, nullptr, hInst, nullptr);
    for (const auto& s : sample)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    if (conflictCount > sample.size()) {
        wchar_t more[96];
        StringCchPrintfW(more, 96, loc::T(loc::S::ConflictMore),
                         conflictCount - (unsigned long long)sample.size());
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)more);
    }

    // Wider buttons: the German labels ("Vorhandene überspringen") do not fit
    // the English widths.
    const int by = 250, bh = 30, bw = 170;
    HWND bReplace = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnReplaceAll),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false), 16, by, bw, bh, hwnd,
        (HMENU)(INT_PTR)ID_REPLACE, hInst, nullptr);
    HWND bNewer = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnOnlyNewer),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(true), 16 + bw + 8, by, bw, bh, hwnd,
        (HMENU)(INT_PTR)ID_NEWER, hInst, nullptr);
    HWND bSkip = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnSkipExisting),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false), 16, by + bh + 8, bw, bh, hwnd,
        (HMENU)(INT_PTR)ID_SKIP, hInst, nullptr);
    HWND bCancel = CreateWindowW(L"BUTTON", loc::T(loc::S::BtnCancel),
        WS_CHILD | WS_VISIBLE | theme::ButtonStyle(false), CW - 116, by + bh + 8, 100, bh,
        hwnd, (HMENU)(INT_PTR)ID_CANCEL, hInst, nullptr);

    SetFont(lblHead, st.fontBold);
    SetFont(lblSub, st.font);
    SetFont(list, st.font);
    SetFont(bReplace, st.font);
    SetFont(bNewer, st.font);
    SetFont(bSkip, st.font);
    SetFont(bCancel, st.font);

    theme::ApplyToWindow(hwnd);
    for (HWND b : {bReplace, bNewer, bSkip, bCancel, list}) theme::ApplyToControl(b);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(bNewer);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (st.fontBold && st.fontBold != st.font) DeleteObject(st.fontBold);
    if (st.font && st.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(st.font);

    cancelled = st.cancelled;
    return st.choice;
}

} // namespace angelcopy
