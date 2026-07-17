#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

// ---- Module-wide COM lifetime -------------------------------------------
extern HMODULE g_hModule;   // set in DllMain
extern long    g_cDllRef;   // outstanding-object count for DllCanUnloadNow

inline void DllAddRef()  { InterlockedIncrement(&g_cDllRef); }
inline void DllRelease() { InterlockedDecrement(&g_cDllRef); }

// ---- Class IDs (defined once in Guids.cpp) ------------------------------
extern const CLSID CLSID_AngelContextMenu;
extern const CLSID CLSID_AngelDragDrop;

// String forms used by the registrar.
#define ANGEL_CONTEXTMENU_CLSID_STR L"{7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D02}"
#define ANGEL_DRAGDROP_CLSID_STR    L"{7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D03}"

// Legacy. AngelCOPY <= 1.0 registered a DropHandler for Directory / Drive /
// Directory\Background, on the false assumption that the shell consults it for
// folder drops. It does not — folder drops come from the shell's own internal
// IDropTarget (verified in tests\test_droptarget.cpp), so the handler was never
// invoked. These are kept ONLY so the registrar can clean the stale keys off
// machines that already have them.
#define ANGEL_DROPHANDLER_CLSID_STR L"{7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D01}"

// ---- Object factories (implemented in their own .cpp) -------------------
HRESULT CreateAngelContextMenu(REFIID riid, void** ppv);
HRESULT CreateAngelDragDropHandler(REFIID riid, void** ppv);

// ---- Shared helpers (angel:: namespace) ---------------------------------
namespace angel {

std::wstring ModuleDir();     // directory containing AngelCopyShell.dll
std::wstring RunnerPath();    // <ModuleDir>\AngelCopyRunner.exe

// Upper-case drive letter of a path, or 0 if it has none (UNC, etc.).
wchar_t DriveLetter(const std::wstring& path);

// Spawn AngelCopyRunner.exe: `op` is L"copy", L"move" or L"delete". `dest` is
// ignored (and omitted from the command line) for delete. Sources are written
// to a temp UTF-16LE list file (consumed+deleted by the runner) to dodge the
// command-line length limit. Returns true if the process launched.
bool LaunchRunner(const wchar_t* op, const std::wstring& dest,
                  const std::vector<std::wstring>& sources);

// ---- CF_HDROP / clipboard ----
bool GetHDropPaths(IDataObject* pdo, std::vector<std::wstring>& out);
DWORD GetPreferredDropEffect(IDataObject* pdo); // 0 if absent

bool ClipboardHasHDrop();
bool GetClipboardHDrop(std::vector<std::wstring>& out, DWORD& effect);

} // namespace angel
