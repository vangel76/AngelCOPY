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
    RGB( 90,  90,  90), // textDim
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
    RGB(170, 170, 170), // textDim
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
    if (!IsDark()) return;
    // Dark title bar. Attribute 20 is the documented one (Windows 10 2004+);
    // 19 was the pre-release value on 1809..1909. Try both, ignore failure.
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

} // namespace theme
