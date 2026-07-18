#pragma once
#include "Common.h"
#include <string>
#include <vector>

// Classic context-menu handler providing "Paste FAST" (on a folder or folder
// background, when the clipboard holds files) and "Delete FAST" (on a
// selection). There is deliberately no "Copy FAST": filling the clipboard is
// exactly what Ctrl+C already does — the speed lives in the paste.
// Labels come from shared/Localize.h. Registered under
// *\shellex\ContextMenuHandlers, Directory\shellex\ContextMenuHandlers and
// Directory\Background\shellex\ContextMenuHandlers.
class CContextMenu : public IShellExtInit, public IContextMenu {
public:
    CContextMenu();

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
    ~CContextMenu();

    long m_ref;
    std::vector<std::wstring> m_sources;  // selected items (Delete FAST target)
    std::wstring m_pasteTarget;           // folder for Paste FAST ("" if none)

    // Clipboard verb at menu-build time: after Ctrl+X the entry reads
    // "Move here FAST", after Ctrl+C "Copy here FAST" — the label must say
    // what will actually happen (invoke re-reads the clipboard anyway).
    bool m_pasteIsMove = false;

    // Command offsets from idCmdFirst; 0xFFFFFFFF when the item isn't shown.
    UINT m_idPaste = 0xFFFFFFFF;
    UINT m_idSync = 0xFFFFFFFF;  // mirror clipboard folders into the target
    UINT m_idDelete = 0xFFFFFFFF;
};
