#pragma once
#include <windows.h>

// Light/dark colours taken from the system setting. Windows does not do this for
// plain Win32 controls: only the static/edit backgrounds we paint ourselves via
// WM_CTLCOLOR* follow along, the title bar needs DwmSetWindowAttribute, and push
// buttons need SetWindowTheme — none of it is automatic.

namespace theme {

struct Colors {
    COLORREF bg;          // dialog background
    COLORREF text;        // primary text
    COLORREF textDim;     // secondary text
    COLORREF chartBg;     // chart plot area
    COLORREF chartGrid;   // grid lines
    COLORREF chartFill;   // progress band (the "bar" half of the chart)
    COLORREF chartSkip;   // band stretches that were skipped, not copied (green)
    COLORREF chartCurve;  // throughput curve, line
    COLORREF chartArea;   // throughput curve, fill under the line
    COLORREF border;      // chart frame
};

// True when Windows is set to dark app mode. Read once and cached.
bool IsDark();

const Colors& C();

// Background brush matching C().bg. Owned by the module; do not delete.
HBRUSH BgBrush();

// Dark title bar + dark-aware child controls. Call after creating the window and
// its children. No-ops on systems/controls that do not support it.
void ApplyToWindow(HWND hwnd);
void ApplyToControl(HWND child);

// Push buttons are the one control Windows will NOT darken: measured, a plain
// BS_PUSHBUTTON stays RGB(240,240,240) in dark mode even after
// SetWindowTheme(L"DarkMode_Explorer"). The usual fix is the undocumented
// uxtheme ordinal 135 (SetPreferredAppMode) — deliberately not used here. In
// dark mode buttons are owner-drawn instead and painted by DrawButton().
//
// Consequence: BS_OWNERDRAW occupies the same style bits as BS_DEFPUSHBUTTON, so
// a dark button cannot be *the* default button. Esc still reaches IDCANCEL via
// IsDialogMessage, and Enter simply does nothing — which keeps the delete
// confirmation safe either way.
DWORD ButtonStyle(bool isDefault);

// Paint an owner-drawn button. Only called when IsDark().
void DrawButton(const DRAWITEMSTRUCT* dis, HFONT font);

} // namespace theme
