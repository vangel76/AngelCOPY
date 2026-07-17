#pragma once
#include "Common.h"
#include <string>
#include <vector>

// Right-drag handler. Registered under Directory\shellex\DragDropHandlers and
// Drive\shellex\DragDropHandlers — the extension point Windows actually honours
// for drag & drop (7-Zip and WinMerge use the same one).
//
// It cannot intercept a plain LEFT drag: the shell serves folder drops from its
// own internal IDropTarget and never consults a registered DropHandler for
// Directory (verified — see tests\test_droptarget.cpp). Only a right-drag, which
// asks for a menu, is extensible without hooking Explorer's API.
//
// On invocation the shell hands us both halves of the operation:
//   pidlFolder -> the folder the items were dropped ON  (destination)
//   pdtobj     -> the dragged items                     (sources)
class CDragDropHandler : public IShellExtInit, public IContextMenu {
public:
    CDragDropHandler();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IShellExtInit
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pdtobj,
                              HKEY hkeyProgID) override;

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
                                    UINT idCmdLast, UINT uFlags) override;
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* pici) override;
    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uType, UINT* pReserved,
                                    CHAR* pszName, UINT cchMax) override;

private:
    ~CDragDropHandler();

    long m_ref;
    std::wstring m_target;               // folder the items were dropped on
    std::vector<std::wstring> m_sources; // dragged items

    UINT m_idCopy = 0xFFFFFFFF;
    UINT m_idMove = 0xFFFFFFFF;
    UINT m_idSync = 0xFFFFFFFF; // shown only when a directory is dragged
};
