#include "DragDropHandler.h"
#include "../shared/Localize.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <new>

#pragma comment(lib, "Shlwapi.lib")

CDragDropHandler::CDragDropHandler() : m_ref(1) { DllAddRef(); }
CDragDropHandler::~CDragDropHandler() { DllRelease(); }

HRESULT CreateAngelDragDropHandler(REFIID riid, void** ppv) {
    *ppv = nullptr;
    CDragDropHandler* p = new (std::nothrow) CDragDropHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

// ---- IUnknown ----
IFACEMETHODIMP CDragDropHandler::QueryInterface(REFIID riid, void** ppv) {
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
IFACEMETHODIMP_(ULONG) CDragDropHandler::AddRef() { return InterlockedIncrement(&m_ref); }
IFACEMETHODIMP_(ULONG) CDragDropHandler::Release() {
    long n = InterlockedDecrement(&m_ref);
    if (n == 0) delete this;
    return n;
}

// ---- IShellExtInit ----
IFACEMETHODIMP CDragDropHandler::Initialize(PCIDLIST_ABSOLUTE pidlFolder,
                                            IDataObject* pdtobj, HKEY) {
    // For a drag&drop handler pidlFolder is the DROP TARGET (not the parent of a
    // selection, as it would be for a plain context menu).
    if (pidlFolder) {
        wchar_t buf[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidlFolder, buf)) m_target = buf;
    }
    if (pdtobj) angel::GetHDropPaths(pdtobj, m_sources);

    // Only handle plain file-system drops onto a real directory.
    if (m_target.empty() || !PathIsDirectoryW(m_target.c_str()) || m_sources.empty())
        return E_INVALIDARG; // shell drops us from the menu
    return S_OK;
}

// ---- IContextMenu ----
IFACEMETHODIMP CDragDropHandler::QueryContextMenu(HMENU hMenu, UINT indexMenu,
                                                  UINT idCmdFirst, UINT idCmdLast,
                                                  UINT uFlags) {
    if (uFlags & CMF_DEFAULTONLY)
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    if (m_target.empty() || m_sources.empty())
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

    UINT id = idCmdFirst, pos = indexMenu, added = 0;

    if (id <= idCmdLast) {
        InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                    loc::T(loc::S::MenuCopyHere));
        m_idCopy = id - idCmdFirst;
        ++id; ++pos; ++added;
    }
    if (id <= idCmdLast) {
        InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                    loc::T(loc::S::MenuMoveHere));
        m_idMove = id - idCmdFirst;
        ++id; ++pos; ++added;
    }

    // Mirror is a folder operation — only offer it when a directory is being
    // dragged. (Loose files would just be a plain copy with a scary name.)
    bool anyDir = false;
    for (const auto& s : m_sources)
        if (PathIsDirectoryW(s.c_str())) { anyDir = true; break; }
    if (anyDir && id <= idCmdLast) {
        InsertMenuW(hMenu, pos, MF_BYPOSITION | MF_STRING, id,
                    loc::T(loc::S::MenuSyncHere));
        m_idSync = id - idCmdFirst;
        ++id; ++pos; ++added;
    }
    return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, added);
}

IFACEMETHODIMP CDragDropHandler::InvokeCommand(CMINVOKECOMMANDINFO* pici) {
    if (!pici) return E_INVALIDARG;
    if (HIWORD(pici->lpVerb) != 0) return E_FAIL; // only menu selections
    UINT offset = LOWORD(pici->lpVerb);

    if (offset == m_idCopy)
        return angel::LaunchRunner(L"copy", m_target, m_sources) ? S_OK : E_FAIL;
    if (offset == m_idMove)
        return angel::LaunchRunner(L"move", m_target, m_sources) ? S_OK : E_FAIL;
    if (offset == m_idSync)
        // The runner prompts (copy + delete summary) before touching anything.
        return angel::LaunchRunner(L"sync", m_target, m_sources) ? S_OK : E_FAIL;
    return E_FAIL;
}

IFACEMETHODIMP CDragDropHandler::GetCommandString(UINT_PTR idCmd, UINT uType,
                                                  UINT*, CHAR* pszName,
                                                  UINT cchMax) {
    const wchar_t* help = nullptr;
    if (idCmd == m_idCopy) help = loc::T(loc::S::HelpCopyHere);
    if (idCmd == m_idMove) help = loc::T(loc::S::HelpMoveHere);
    if (idCmd == m_idSync) help = loc::T(loc::S::HelpSyncHere);
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
