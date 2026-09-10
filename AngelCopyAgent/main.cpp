// AngelCopyAgent.exe — the Ctrl+V accelerator (tray agent).
//
// Windows offers NO supported shell hook for Ctrl+V in Explorer: the paste
// always runs Explorer's own engine. The one clean route — designed July 2026,
// initially declined, green-lit later — is this PowerToys-style background
// agent with a low-level keyboard hook:
//
//   Ctrl+V pressed
//     AND the foreground window is an Explorer file window (CabinetWClass)
//     AND the clipboard holds files (CF_HDROP)
//     AND focus is NOT in an edit control (address bar, search, F2-rename —
//         pasting TEXT there must stay native)
//   -> swallow the keystroke, resolve the window's folder, launch the runner
//      (copy or move per the clipboard's PreferredDropEffect, like Paste FAST).
//
// FAIL-OPEN is the design law here:
//   - The hook callback does only cheap user32 checks (class name, clipboard
//     format probe). COM/folder resolution happens on the main thread AFTER
//     the swallow; if it fails (virtual folder, zip, anything), the agent
//     REPLAYS Ctrl+V via SendInput and Windows pastes as if we never existed.
//     The hook ignores injected events (LLKHF_INJECTED), so the replay cannot
//     loop.
//   - If the callback ever stalls, Windows silently unhooks us -> native
//     Ctrl+V again. If the agent isn't running -> native Ctrl+V. There is no
//     failure mode that loses the keystroke.
//
// Tray icon: toggle interception, exit. Single instance via named mutex.
// --test-resolve: prints every open Explorer window's resolved folder path to
// the parent console and exits (headless verification of the COM resolution).

#include "../AngelCopyShell/Common.h"
#include "../shared/Localize.h"
#include "../shared/Util.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>     // IShellWindows, IWebBrowser2
#include <shobjidl.h>   // IShellBrowser, IFolderView, IPersistFolder2
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

constexpr UINT WM_APP_PASTE = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_DEBUG = WM_APP + 3; // wParam = reason code (see DbgLog)
constexpr UINT WM_APP_DELETE = WM_APP + 4;
constexpr UINT WM_APP_PROPS = WM_APP + 5; // Alt+Enter -> fast properties
constexpr UINT ID_TRAY_TOGGLE = 1;
constexpr UINT ID_TRAY_EXIT = 2;
constexpr UINT ID_TRAY_GUIDE = 3;

HWND g_msgWnd = nullptr;
HHOOK g_hook = nullptr;
volatile LONG g_enabled = 1;

// Debug tracing, opt-in: if %TEMP%\acp_agent_debug.on exists at startup, every
// Ctrl+V decision is appended to %TEMP%\acp_agent.log. The hook only
// PostMessages a reason code (cheap, safe); the main thread writes the file so
// the hook callback stays fast (file I/O in an LL hook risks removal).
bool g_debug = false;

void DbgLog(WPARAM reason) {
    static const wchar_t* kMsg[] = {
        L"INTERCEPT (explorer + files + not edit)",
        L"pass: foreground not an Explorer window",
        L"pass: clipboard has no files (no CF_HDROP)",
        L"pass: focus is in an edit control (text paste)",
    };
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"acp_agent.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d  Ctrl+V  ->  %s\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds,
                 reason < ARRAYSIZE(kMsg) ? kMsg[reason] : L"?");
        fclose(f);
    }
}

// ---- hook side (must stay cheap: no COM, no blocking) ----------------------

bool ClassIs(HWND w, const wchar_t* name) {
    wchar_t cls[64]{};
    GetClassNameW(w, cls, 64);
    return _wcsicmp(cls, name) == 0;
}

// A foreground Explorer file window whose focus is NOT in an edit control
// (address bar, search box, F2-rename — those keep native behavior). Shared
// gate for both shortcuts. Returns the window, or nullptr.
HWND ExplorerTargetOrNull() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    if (!ClassIs(fg, L"CabinetWClass") && !ClassIs(fg, L"ExploreWClass"))
        return nullptr;
    GUITHREADINFO gti{sizeof(gti)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti))
        return nullptr; // can't tell where focus is -> fail safe: native paste
    if (gti.hwndFocus) {
        wchar_t cls[64]{};
        GetClassNameW(gti.hwndFocus, cls, 64);
        CharUpperW(cls);
        if (wcsstr(cls, L"EDIT")) return nullptr; // Edit / RichEdit variants
    }
    return fg;
}

// Ctrl+V decision. Returns a reason code (0 = intercept; >0 = pass through,
// see DbgLog), and on 0 fills outTarget. Also needs files on the clipboard.
WPARAM InterceptReason(HWND* outTarget) {
    HWND fg = ExplorerTargetOrNull();
    if (!fg) {
        // Distinguish "not explorer" from "edit focus" only loosely here; the
        // debug log's finer reasons come from the checks above being folded.
        HWND raw = GetForegroundWindow();
        if (raw && (ClassIs(raw, L"CabinetWClass") ||
                    ClassIs(raw, L"ExploreWClass")))
            return 3; // explorer but edit-focused
        return 1;     // not an explorer window
    }
    if (!IsClipboardFormatAvailable(CF_HDROP)) return 2;
    *outTarget = fg;
    return 0;
}

HWND g_pasteTarget = nullptr;  // set by the hook, read by the main thread
HWND g_deleteTarget = nullptr;
HWND g_propsTarget = nullptr;

// Key-repeat suppression. Holding Ctrl+V / Shift+Del auto-repeats WM_KEYDOWN;
// without this each repeat launched another copy process / delete dialog on the
// same target. Set when we swallow a keydown, cleared on the matching keyup.
LONG g_vHeld = 0;
LONG g_delHeld = 0;
LONG g_enterHeld = 0;

// Modifier snapshot for the chord checks below. Same cheap GetAsyncKeyState
// calls as before, just factored — the hook-callback cheapness law holds.
struct Mods {
    bool ctrl, shift, alt, win;
};
Mods ReadMods() {
    return {(GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0,
            ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
                0};
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kk = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        // Clear the held-flags OUTSIDE the enabled gate. If the user unticks
        // "Enabled" in the tray while a key is still down, the keyup would
        // otherwise never be seen and the flag would strand at 1 — swallowing
        // the next Ctrl+V silently, with nothing launched.
        if ((wp == WM_KEYUP || wp == WM_SYSKEYUP) && !(kk->flags & LLKHF_INJECTED)) {
            if (kk->vkCode == 'V') {
                if (InterlockedExchange(&g_vHeld, 0) == 1) return 1; // balance
            } else if (kk->vkCode == VK_DELETE) {
                if (InterlockedExchange(&g_delHeld, 0) == 1) return 1;
            } else if (kk->vkCode == VK_RETURN) {
                if (InterlockedExchange(&g_enterHeld, 0) == 1) return 1;
            }
        }
    }
    if (nCode == HC_ACTION && InterlockedCompareExchange(&g_enabled, 0, 0)) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && k->vkCode == 'V' &&
            !(k->flags & LLKHF_INJECTED)) {          // never touch our replay
            const Mods m = ReadMods();
            if (m.ctrl && !m.shift && !m.alt && !m.win) {
                HWND target = nullptr;
                WPARAM reason = InterceptReason(&target);
                if (g_debug) PostMessageW(g_msgWnd, WM_APP_DEBUG, reason, 0);
                if (reason == 0) {
                    // Swallow auto-repeats without re-launching: post only on
                    // the first keydown of a physical press.
                    if (InterlockedExchange(&g_vHeld, 1) == 0) {
                        g_pasteTarget = target;
                        PostMessageW(g_msgWnd, WM_APP_PASTE, 0, 0);
                    }
                    return 1; // swallow: heavy lifting happens off the hook
                }
            }
        }
        // (keyup balancing for V/Delete happens above, outside this gate)
        // Shift+Delete -> permanent delete of the current selection via our
        // engine (confirmation + parallel delete). Only files, never edit
        // focus (Shift+Del there deletes text). Selection is fetched on the
        // main thread; if there is none the keystroke is replayed native.
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && k->vkCode == VK_DELETE &&
            !(k->flags & LLKHF_INJECTED)) {
            const Mods m = ReadMods();
            if (m.shift && !m.ctrl && !m.alt && !m.win) {
                HWND target = ExplorerTargetOrNull();
                if (target) {
                    if (InterlockedExchange(&g_delHeld, 1) == 0) {
                        g_deleteTarget = target;
                        PostMessageW(g_msgWnd, WM_APP_DELETE, 0, 0);
                    }
                    return 1;
                }
            }
        }
        // Alt+Enter -> fast properties of the Explorer selection (or the
        // current folder when nothing is selected). Arrives as WM_SYSKEYDOWN
        // because Alt is held. Same laws as the other two: cheap checks only,
        // resolution on the main thread, single files and failures are
        // replayed native.
        if (wp == WM_SYSKEYDOWN && k->vkCode == VK_RETURN &&
            !(k->flags & LLKHF_INJECTED)) {
            const Mods m = ReadMods();
            if (m.alt && !m.ctrl && !m.shift && !m.win) {
                HWND target = ExplorerTargetOrNull();
                if (target) {
                    if (InterlockedExchange(&g_enterHeld, 1) == 0) {
                        g_propsTarget = target;
                        PostMessageW(g_msgWnd, WM_APP_PROPS, 0, 0);
                    }
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

// ---- main-thread side ------------------------------------------------------

// Give a swallowed chord back to Windows: inject modifier+key (the modifier
// is usually still physically held; inject a down anyway for the
// released-early case). The hook ignores injected events (LLKHF_INJECTED),
// so the replay reaches Explorer untouched and cannot loop.
void ReplayChord(WORD mod, WORD key) {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = mod;
    in[1].ki.wVk = key;
    in[2].ki.wVk = key;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = mod;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

void ReplayCtrlV() { ReplayChord(VK_CONTROL, 'V'); }

// The active IShellView of the Explorer window `target` (AddRef'd, caller
// releases), or nullptr if `target` isn't a shell-browser window. Both the
// paste (current folder) and the delete (selection) paths go through this.
IShellView* GetActiveShellView(HWND target) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&sw))))
        return nullptr;

    IShellView* found = nullptr;
    long count = 0;
    sw->get_Count(&count);
    for (long i = 0; i < count && !found; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(sw->Item(vi, &disp)) || !disp) continue;

        IWebBrowser2* wb = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&wb)))) {
            SHANDLE_PTR hwndVal = 0;
            if (SUCCEEDED(wb->get_HWND(&hwndVal)) && (HWND)hwndVal == target) {
                IServiceProvider* sp = nullptr;
                if (SUCCEEDED(wb->QueryInterface(IID_PPV_ARGS(&sp)))) {
                    IShellBrowser* sb = nullptr;
                    if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser,
                                                   IID_PPV_ARGS(&sb)))) {
                        sb->QueryActiveShellView(&found); // AddRef's on success
                        sb->Release();
                    }
                    sp->Release();
                }
            }
            wb->Release();
        }
        disp->Release();
    }
    sw->Release();
    return found;
}

void ReplayShiftDelete() { ReplayChord(VK_SHIFT, VK_DELETE); }

// Filesystem path of the folder shown in Explorer window `target`; L"" for
// virtual locations (This PC, zips, ...) — those get the native paste.
std::wstring ResolveExplorerFolder(HWND target) {
    std::wstring result;
    IShellView* sv = GetActiveShellView(target);
    if (!sv) return result;
    IFolderView* fv = nullptr;
    if (SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
        IPersistFolder2* pf = nullptr;
        if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf)))) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                wchar_t* name = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH,
                                                  &name)) &&
                    name) {
                    result = name;
                    CoTaskMemFree(name);
                }
                CoTaskMemFree(pidl);
            }
            pf->Release();
        }
        fv->Release();
    }
    sv->Release();
    return result;
}

// Filesystem paths of the SELECTED items in Explorer window `target`. Empty
// when nothing is selected or the items aren't real files (virtual view) —
// the caller then replays native Shift+Delete.
bool ResolveExplorerSelection(HWND target, std::vector<std::wstring>& out) {
    IShellView* sv = GetActiveShellView(target);
    if (!sv) return false;
    IDataObject* pdo = nullptr;
    bool ok = false;
    if (SUCCEEDED(sv->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&pdo))) &&
        pdo) {
        ok = angel::GetHDropPaths(pdo, out); // CF_HDROP -> filesystem paths
        pdo->Release();
    }
    sv->Release();
    return ok && !out.empty();
}

void DoPaste() {
    HWND target = g_pasteTarget;
    std::wstring folder = target ? ResolveExplorerFolder(target) : L"";
    if (folder.empty()) { ReplayCtrlV(); return; } // virtual folder: native

    std::vector<std::wstring> paths;
    DWORD effect = 0;
    if (!angel::GetClipboardHDrop(paths, effect) || paths.empty()) {
        ReplayCtrlV();
        return;
    }
    const bool move = (effect & DROPEFFECT_MOVE) != 0;
    if (!angel::LaunchRunner(move ? L"move" : L"copy", folder, paths)) {
        ReplayCtrlV();
        return;
    }
    if (move) {
        // A cut is consumed by its paste (Explorer does the same) — a second
        // Ctrl+V must not try to move the now-gone sources again.
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        }
    }
}

void DoDelete() {
    HWND target = g_deleteTarget;
    std::vector<std::wstring> paths;
    if (!target || !ResolveExplorerSelection(target, paths)) {
        ReplayShiftDelete(); // no usable selection: native Shift+Delete
        return;
    }
    // The runner shows the mandatory delete confirmation before removing
    // anything (permanent, no Recycle Bin), then deletes in parallel.
    if (!angel::LaunchRunner(L"delete", L"", paths))
        ReplayShiftDelete();
}

void ReplayAltEnter() { ReplayChord(VK_MENU, VK_RETURN); }

// Directory test that survives >MAX_PATH (the shell hands us plain paths).
bool IsDirLong(const std::wstring& p) {
    DWORD a = GetFileAttributesW(acutil::ExtLongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void DoProps() {
    HWND target = g_propsTarget;
    if (!target) { ReplayAltEnter(); return; }
    std::vector<std::wstring> paths;
    if (!ResolveExplorerSelection(target, paths)) {
        // Nothing selected: Explorer shows the CURRENT folder's properties —
        // so do we, with our fast counter.
        std::wstring folder = ResolveExplorerFolder(target);
        if (folder.empty()) { ReplayAltEnter(); return; } // virtual: native
        paths.assign(1, folder);
    } else if (paths.size() == 1 && !IsDirLong(paths[0])) {
        // A single FILE has nothing to count — the native sheet is instant
        // and has all the tabs. Deliberate pass-through.
        ReplayAltEnter();
        return;
    }
    if (!angel::LaunchRunner(L"props", L"", paths))
        ReplayAltEnter();
}

// ---- tray ------------------------------------------------------------------

void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    // Our own icon (resource id 1, shared\AppIcon.rc) at the tray's small
    // size; the stock IDI_APPLICATION fallback only if the resource is
    // missing (hand-built exe without the .res).
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                  IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, loc::T(loc::S::TrayTooltip), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Open the installed quick guide next to the agent's exe. Language picks the
// file the installer shipped; if one is missing (hand-copied install), the
// other is tried — better any guide than none.
void OpenGuide() {
    const bool de =
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    std::wstring dir = angel::ModuleDir();
    const wchar_t* first = de ? L"AngelCOPY Anleitung.txt" : L"AngelCOPY Guide.txt";
    const wchar_t* other = de ? L"AngelCOPY Guide.txt" : L"AngelCOPY Anleitung.txt";
    std::wstring path = dir + L"\\" + first;
    if (!PathFileExistsW(path.c_str())) path = dir + L"\\" + other;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void TrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TRAY_TOGGLE,
                loc::T(loc::S::TrayEnabled));
    AppendMenuW(m, MF_STRING, ID_TRAY_GUIDE, loc::T(loc::S::TrayGuide));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, loc::T(loc::S::TrayExit));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required or the menu won't dismiss properly
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

// Pop a "done" balloon on our persistent tray icon. Sent cross-process by the
// runner (WM_COPYDATA) when a transfer finishes — the runner has no tray icon
// of its own and exits immediately, so the notification lives here.
void ShowDoneBalloon(const wchar_t* body) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, L"AngelCOPY", ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, body, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Windows silently REMOVES a WH_KEYBOARD_LL hook whose callback once exceeds
// LowLevelHooksTimeout (system load, a disk waking up — one slow tick in days
// of uptime suffices). The agent then looks alive (tray, balloons) but Ctrl+V
// and Shift+Del are native again. There is no API to ask whether the hook
// still exists, so it is re-registered periodically — the PowerToys approach.
// Unhook+rehook is microseconds; the interval is the worst-case dead window.
constexpr UINT_PTR kRehookTimer = 1;
constexpr UINT     kRehookMs = 30 * 1000;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        // Branch on the timer id (see the ProgressUI WM_TIMER gotcha).
        if (wp == kRehookTimer) {
            if (g_hook) UnhookWindowsHookEx(g_hook);
            g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                       GetModuleHandleW(nullptr), 0);
            return 0;
        }
        break;
    case WM_COPYDATA: {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lp;
        if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
            // Trust only our own message shape; NUL-terminate defensively.
            std::wstring body((const wchar_t*)cds->lpData,
                              cds->cbData / sizeof(wchar_t));
            body.push_back(L'\0');
            ShowDoneBalloon(body.c_str());
        }
        return TRUE;
    }
    case WM_APP_PASTE:
        DoPaste();
        return 0;
    case WM_APP_DELETE:
        DoDelete();
        return 0;
    case WM_APP_PROPS:
        DoProps();
        return 0;
    case WM_APP_DEBUG:
        DbgLog(wp);
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            TrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_TOGGLE) {
            InterlockedExchange(&g_enabled, g_enabled ? 0 : 1);
        } else if (LOWORD(wp) == ID_TRAY_GUIDE) {
            OpenGuide();
        } else if (LOWORD(wp) == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        TrayRemove(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    // Explorer (re)built the taskbar: every tray icon was discarded and must
    // be re-added NOW. Without this the icon vanishes on any Explorer restart
    // — including the one the installer itself performs right after starting
    // the agent (hook kept working, icon gone; happened live, Sep 2026).
    static const UINT kTaskbarCreated =
        RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == kTaskbarCreated) {
        TrayAdd(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- --test-resolve: headless verification of the COM plumbing -------------

int TestResolve() {
    // GUI subsystem: stdout doesn't reach a caller's pipeline — write a file
    // next to the temp dir instead (headless verification).
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring outPath = std::wstring(tmp) + L"acp_agent_test.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, outPath.c_str(), L"w");
    if (f) {
        fclose(f);
        freopen_s(&f, "CONOUT$", "w", stdout); // still try the console
        FILE* rf = nullptr;
        _wfreopen_s(&rf, outPath.c_str(), L"w", stdout);
    }
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&sw)))) {
        printf("no IShellWindows\n");
        return 1;
    }
    long count = 0;
    sw->get_Count(&count);
    printf("shell windows: %ld\n", count);
    for (long i = 0; i < count; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(sw->Item(vi, &disp)) || !disp) continue;
        IWebBrowser2* wb = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&wb)))) {
            SHANDLE_PTR h = 0;
            wb->get_HWND(&h);
            std::wstring p = ResolveExplorerFolder((HWND)h);
            printf("  hwnd=%p  path=%ws\n", (void*)h,
                   p.empty() ? L"<virtual>" : p.c_str());
            std::vector<std::wstring> sel;
            if (ResolveExplorerSelection((HWND)h, sel)) {
                printf("    selection: %zu item(s)\n", sel.size());
                for (auto& s : sel) printf("      %ws\n", s.c_str());
            } else {
                printf("    selection: <none>\n");
            }
            wb->Release();
        }
        disp->Release();
    }
    sw->Release();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    if (cmdLine && wcsstr(cmdLine, L"--test-resolve")) {
        int rc = TestResolve();
        CoUninitialize();
        return rc;
    }

    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_debug = PathFileExistsW((std::wstring(tmp) + L"acp_agent_debug.on").c_str());
    }

    // Single instance: a second launch (e.g. installer re-run) just exits.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"AngelCopyAgentSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex); // a handle is returned even when it exists
        CoUninitialize();
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"AngelCopyAgentWnd";
    RegisterClassW(&wc);
    // One hidden top-level window (NOT message-only: the tray callback needs a
    // real window). Do NOT create-then-destroy a throwaway first — its
    // WM_DESTROY posts WM_QUIT, which then ends the message loop instantly.
    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                               L"AngelCopyAgent", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, hInst, nullptr);
    if (!g_msgWnd) { CoUninitialize(); return 1; }
    TrayAdd(g_msgWnd);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    // No hook -> agent is useless but harmless; keep the tray so the user can
    // see it and exit. Ctrl+V stays native either way (fail-open).
    // Periodic re-hook (see kRehookTimer): recovers from Windows silently
    // dropping the hook after one slow callback.
    SetTimer(g_msgWnd, kRehookTimer, kRehookMs, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
