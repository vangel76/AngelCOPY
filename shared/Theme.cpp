#include "Theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "UxTheme.lib")

namespace theme {

namespace {

const Colors kLight = {
    RGB(255, 255, 255), // bg
    RGB(  0,   0,   0), // text
    RGB(250, 250, 250), // chartBg
    RGB(220, 220, 220), // chartGrid
    RGB(204, 232, 255), // chartFill  (progress band, pale accent)
    RGB(183, 226, 190), // chartSkip  (skipped stretches, pale green)
    RGB(  0, 120, 215), // chartCurve (Windows accent blue)
    RGB(150, 205, 245), // chartArea
    RGB(180, 180, 180), // border
};

const Colors kDark = {
    RGB( 32,  32,  32), // bg
    RGB(240, 240, 240), // text
    RGB( 45,  45,  45), // chartBg
    RGB( 70,  70,  70), // chartGrid
    RGB( 45,  85, 115), // chartFill
    RGB( 44, 100,  58), // chartSkip
    RGB( 90, 190, 245), // chartCurve
    RGB( 60, 140, 190), // chartArea
    RGB( 80,  80,  80), // border
};

HBRUSH g_brush = nullptr;

} // namespace

bool IsDark() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;

    // Test override, so the other theme can be checked without touching the
    // user's actual Windows setting: ANGELCOPY_THEME=light|dark.
    wchar_t env[16]{};
    if (GetEnvironmentVariableW(L"ANGELCOPY_THEME", env, 16)) {
        if (_wcsicmp(env, L"dark") == 0)  { cached = 1; return true; }
        if (_wcsicmp(env, L"light") == 0) { cached = 0; return false; }
    }

    cached = 0; // default: light, matching Windows' own default
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &h) == ERROR_SUCCESS) {
        DWORD val = 1, cb = sizeof(val), type = 0;
        if (RegQueryValueExW(h, L"AppsUseLightTheme", nullptr, &type,
                             reinterpret_cast<BYTE*>(&val), &cb) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            cached = (val == 0) ? 1 : 0;
        }
        RegCloseKey(h);
    }
    return cached == 1;
}

const Colors& C() { return IsDark() ? kDark : kLight; }

HBRUSH BgBrush() {
    if (!g_brush) g_brush = CreateSolidBrush(C().bg);
    return g_brush;
}

void ApplyToWindow(HWND hwnd) {
    // Title-bar app icon: the window classes carry no hIcon, so every dialog
    // showed the generic document glyph. LR_SHARED: the system caches the
    // icon, nothing to free.
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HICON icoBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON), LR_SHARED);
    // Not named "small": rpcndr.h #defines small as char.
    HICON icoSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (icoBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icoBig);
    if (icoSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icoSmall);

    // Brand-orange caption so AngelCOPY windows are spottable at a glance.
    // DWMWA_CAPTION_COLOR (35) / DWMWA_TEXT_COLOR (36) are Windows 11; the
    // calls fail silently elsewhere. Same orange as the app icon (#EE7A1A),
    // white caption text for contrast — in BOTH themes (that is the point).
    COLORREF caption = RGB(238, 122, 26);
    COLORREF captionText = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &captionText, sizeof(captionText));

    if (!IsDark()) return;
    // Dark title bar. Attribute 20 is the documented one (Windows 10 2004+);
    // 19 was the pre-release value on 1809..1909. Try both, ignore failure.
    // (With the orange caption above this mainly keeps menus/frame remnants
    // consistent on systems where 35 is unsupported.)
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

void ApplyToControl(HWND child) {
    if (!IsDark()) return;
    // Helps edits/scrollbars. Push buttons ignore it — see DrawButton().
    SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
}

DWORD ButtonStyle(bool isDefault) {
    if (IsDark()) return BS_OWNERDRAW;
    return isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON;
}

void DrawButton(const DRAWITEMSTRUCT* dis, HFONT font) {
    const Colors& c = C();
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    COLORREF face = pressed ? RGB(80, 80, 80) : RGB(60, 60, 60);
    if (disabled) face = RGB(45, 45, 45);

    RECT r = dis->rcItem;
    HBRUSH b = CreateSolidBrush(face);
    FillRect(dis->hDC, &r, b);
    DeleteObject(b);

    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1,
                         focused ? c.chartCurve : RGB(105, 105, 105));
    HGDIOBJ op = SelectObject(dis->hDC, pen);
    HGDIOBJ ob = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(dis->hDC, op);
    SelectObject(dis->hDC, ob);
    DeleteObject(pen);

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? RGB(125, 125, 125) : c.text);
    HGDIOBJ of = SelectObject(dis->hDC, font);
    DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, of);
}

// ---- shared dialog boilerplate --------------------------------------------

UiFonts::UiFonts() {
    normal = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        normal = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW b = ncm.lfMessageFont;
        b.lfWeight = FW_SEMIBOLD;
        bold = CreateFontIndirectW(&b);
    }
    if (!bold) bold = normal;
}

UiFonts::~UiFonts() {
    if (bold && bold != normal) DeleteObject(bold);
    if (normal && normal != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(normal);
}

HWND CreateCenteredWindow(WNDPROC proc, const wchar_t* className,
                          const wchar_t* caption, int cw, int ch, DWORD style,
                          DWORD exStyle) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = BgBrush();
    wc.lpszClassName = className;
    RegisterClassW(&wc); // idempotent: re-registration fails harmlessly

    // cw/ch are CLIENT dimensions; the window size must come from
    // AdjustWindowRectEx (mixing the two once hid a Close button behind the
    // report box — caption + borders are ~39px).
    RECT rc{0, 0, cw, ch};
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;
    return CreateWindowExW(exStyle, className, caption, style, sx, sy, W, H,
                           nullptr, nullptr, hInst, nullptr);
}

void RunModalLoop(HWND hwnd) {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

void SetFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

bool LoadWindowPos(int& x, int& y, int w, int h) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AngelCOPY", 0, KEY_READ,
                      &k) != ERROR_SUCCESS)
        return false;
    DWORD vx = 0, vy = 0, cb = sizeof(DWORD), type = 0;
    bool ok =
        RegQueryValueExW(k, L"WindowX", nullptr, &type, (BYTE*)&vx, &cb) ==
            ERROR_SUCCESS && type == REG_DWORD &&
        (cb = sizeof(DWORD),
         RegQueryValueExW(k, L"WindowY", nullptr, &type, (BYTE*)&vy, &cb) ==
             ERROR_SUCCESS) && type == REG_DWORD;
    RegCloseKey(k);
    if (!ok) return false;
    x = (int)(LONG)vx;
    y = (int)(LONG)vy;
    // The saved point must still land on a live monitor — a remembered spot
    // on an unplugged screen would open the window invisibly off-desktop.
    RECT r{x, y, x + w, y + h};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

void SaveWindowPos(HWND hwnd) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return;
    if (IsIconic(hwnd)) return; // minimized coords are (-32000,-32000) junk
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AngelCOPY", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD vx = (DWORD)(LONG)r.left, vy = (DWORD)(LONG)r.top;
    RegSetValueExW(k, L"WindowX", 0, REG_DWORD, (const BYTE*)&vx, sizeof(vx));
    RegSetValueExW(k, L"WindowY", 0, REG_DWORD, (const BYTE*)&vy, sizeof(vy));
    RegCloseKey(k);
}

} // namespace theme
