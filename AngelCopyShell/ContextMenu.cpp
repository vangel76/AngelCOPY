#include "ContextMenu.h"
#include "../shared/Localize.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <new>

#pragma comment(lib, "Shlwapi.lib")

CContextMenu::CContextMenu() : m_ref(1) { DllAddRef(); }
CContextMenu::~CContextMenu() { DllRelease(); }

HRESULT CreateAngelContextMenu(REFIID riid, void** ppv) {
    *ppv = nullptr;
    CContextMenu* p = new (std::nothrow) CContextMenu();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

// ---- IUnknown ----
IFACEMETHODIMP CContextMenu::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
        *ppv = static_cast<IShellExtInit*>(this);
    } else if (riid == IID_IContextMenu) {
        *ppv = static_cast<IContextMenu*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
IFACEMETHODIMP_(ULONG) CContextMenu::AddRef() { return InterlockedIncrement(&m_ref); }
IFACEMETHODIMP_(ULONG) CContextMenu::Release() {
    long n = InterlockedDecrement(&m_ref);
    if (n == 0) delete this;
    return n;
}

// ---- IShellExtInit ----
IFACEMETHODIMP CContextMenu::Initialize(PCIDLIST_ABSOLUTE pidlFolder,
                                        IDataObject* pdtobj, HKEY) {
    if (pdtobj) angel::GetHDropPaths(pdtobj, m_sources);

    std::wstring folderFromPidl;
    if (pidlFolder) {
        wchar_t buf[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidlFolder, buf)) folderFromPidl = buf;
    }

    // Choose a paste target: a single selected directory, else the background
    // folder (when nothing is selected).
    if (m_sources.size() == 1 && PathIsDirectoryW(m_sources[0].c_str())) {
        m_pasteTarget = m_sources[0];
    } else if (m_sources.empty() && !folderFromPidl.empty()) {
        m_pasteTarget = folderFromPidl;
    }
    return S_OK;
}

// ---- IContextMenu ----
IFACEMETHODIMP CContextMenu::QueryContextMenu(HMENU hMenu, UINT indexMenu,
                                              UINT idCmdFirst, UINT idCmdLast,
                                              UINT uFlags) {
    if (uFlags & CMF_DEFAULTONLY)
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

    UINT id = idCmdFirst;
    UINT pos = indexMenu;
    UINT added = 0;

    // No "Copy FAST": putting paths on the clipboard is what Ctrl+C already
    // does, byte for byte (CF_HDROP + PreferredDropEffect) — the speed lives
    // entirely in the paste. Removed as pointless in July 2026.

    std::vector<std::wstring> clip;
    DWORD effect = 0;
    if (!m_pasteTarget.empty() && id <= idCmdLast &&
        angel::GetClipboardHDrop(clip, effect) && !clip.empty()) {
        // The label states the verb, not a generic "Paste": after Ctrl+X this
        // entry MOVES, after Ctrl+C it copies — the user must see which one
        // they are about to get. (Invoke re-reads the clipboard, so a stale
        // label can't change what actually happens — it only mislabels; both
        // read the same PreferredDropEffect.)
        m_pasteIsMove = (effect & DROPEFFECT_MOVE) != 0;
        InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                    loc::T(m_pasteIsMove ? loc::S::MenuMoveHere
                                         : loc::S::MenuCopyHere));
        m_idPaste = id - idCmdFirst;
        ++id; ++pos; ++added;

        // Mirror: only next to the paste entry, and only when the clipboard
        // holds at least one folder — mirroring is a folder operation.
        if (id <= idCmdLast) {
            for (const auto& p : clip) {
                if (PathIsDirectoryW(p.c_str())) {
                    InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                                loc::T(loc::S::MenuSyncHere));
                    m_idSync = id - idCmdFirst;
                    ++id; ++pos; ++added;
                    break;
                }
            }
        }
    }

    if (!m_sources.empty() && id <= idCmdLast) {
        InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                    loc::T(loc::S::MenuDelete));
        m_idDelete = id - idCmdFirst;
        ++id; ++pos; ++added;
    }

    return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, added);
}

IFACEMETHODIMP CContextMenu::InvokeCommand(CMINVOKECOMMANDINFO* pici) {
    if (!pici) return E_INVALIDARG;
    // Only integer verbs are supported (menu selection).
    if (HIWORD(pici->lpVerb) != 0) return E_FAIL;
    UINT offset = LOWORD(pici->lpVerb);

    if (offset == m_idPaste) {
        std::vector<std::wstring> paths;
        DWORD effect = 0;
        if (!angel::GetClipboardHDrop(paths, effect) || paths.empty())
            return E_FAIL;
        bool move = (effect & DROPEFFECT_MOVE) != 0;
        return angel::LaunchRunner(move ? L"move" : L"copy", m_pasteTarget, paths)
                   ? S_OK
                   : E_FAIL;
    }
    if (offset == m_idSync) {
        std::vector<std::wstring> paths;
        DWORD effect = 0;
        if (!angel::GetClipboardHDrop(paths, effect) || paths.empty())
            return E_FAIL;
        // The runner prompts (copy + delete summary) before touching anything.
        // A cut (move effect) is ignored: mirroring never empties the source.
        return angel::LaunchRunner(L"sync", m_pasteTarget, paths) ? S_OK
                                                                  : E_FAIL;
    }
    if (offset == m_idDelete) {
        // The runner prompts before touching anything; never delete from here.
        return angel::LaunchRunner(L"delete", L"", m_sources) ? S_OK : E_FAIL;
    }
    return E_FAIL;
}

IFACEMETHODIMP CContextMenu::GetCommandString(UINT_PTR idCmd, UINT uType, UINT*,
                                              CHAR* pszName, UINT cchMax) {
    if (uType != GCS_VERBW && uType != GCS_HELPTEXTW &&
        uType != GCS_VERBA && uType != GCS_HELPTEXTA)
        return E_INVALIDARG;

    const wchar_t* help = nullptr;
    if (idCmd == m_idPaste)
        help = loc::T(m_pasteIsMove ? loc::S::HelpMoveHere
                                    : loc::S::HelpCopyHere);
    if (idCmd == m_idSync)   help = loc::T(loc::S::HelpSyncHere);
    if (idCmd == m_idDelete) help = loc::T(loc::S::HelpDelete);
    if (!help) return E_INVALIDARG;

    if (uType == GCS_HELPTEXTW) {
        StringCchCopyW(reinterpret_cast<wchar_t*>(pszName), cchMax, help);
        return S_OK;
    }
    if (uType == GCS_HELPTEXTA) {
        WideCharToMultiByte(CP_ACP, 0, help, -1, pszName, cchMax, nullptr, nullptr);
        return S_OK;
    }
    return E_NOTIMPL;
}
