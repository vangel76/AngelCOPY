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
constexpr UINT ID_TRAY_TOGGLE = 1;
constexpr UINT ID_TRAY_EXIT = 2;

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
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) &&
        gti.hwndFocus) {
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

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && InterlockedCompareExchange(&g_enabled, 0, 0)) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && k->vkCode == 'V' &&
            !(k->flags & LLKHF_INJECTED)) {          // never touch our replay
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const bool win = ((GetAsyncKeyState(VK_LWIN) |
                               GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
            if (ctrl && !shift && !alt && !win) {
                HWND target = nullptr;
                WPARAM reason = InterceptReason(&target);
                if (g_debug) PostMessageW(g_msgWnd, WM_APP_DEBUG, reason, 0);
                if (reason == 0) {
                    g_pasteTarget = target;
                    PostMessageW(g_msgWnd, WM_APP_PASTE, 0, 0);
                    return 1; // swallow: heavy lifting happens off the hook
                }
            }
        }
        // Shift+Delete -> permanent delete of the current selection via our
        // engine (confirmation + parallel delete). Only files, never edit
        // focus (Shift+Del there deletes text). Selection is fetched on the
        // main thread; if there is none the keystroke is replayed native.
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && k->vkCode == VK_DELETE &&
            !(k->flags & LLKHF_INJECTED)) {
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const bool win = ((GetAsyncKeyState(VK_LWIN) |
                               GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
            if (shift && !ctrl && !alt && !win) {
                HWND target = ExplorerTargetOrNull();
                if (target) {
                    g_deleteTarget = target;
                    PostMessageW(g_msgWnd, WM_APP_DELETE, 0, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

// ---- main-thread side ------------------------------------------------------

// Give the keystroke back to Windows: inject Ctrl+V (Ctrl is usually still
// physically held; inject a down anyway for the released-early case). The
// hook ignores injected events, so this reaches Explorer untouched.
void ReplayCtrlV() {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'V';
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

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

// Give Shift+Delete back to Windows (native permanent delete). Shift is
// usually still held; inject it anyway for the released-early case. The hook
// ignores injected events, so this reaches Explorer untouched.
void ReplayShiftDelete() {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_SHIFT;
    in[1].ki.wVk = VK_DELETE;
    in[2].ki.wVk = VK_DELETE;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_SHIFT;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

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

// ---- tray ------------------------------------------------------------------

void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

void TrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TRAY_TOGGLE,
                loc::T(loc::S::TrayEnabled));
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
        } else if (LOWORD(wp) == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        TrayRemove(hwnd);
        PostQuitMessage(0);
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
