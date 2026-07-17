#include "Common.h"
#include <shlwapi.h>
#include <string>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Advapi32.lib")

namespace {

// Right-drag menu. This is the extension point the shell actually honours for
// drag & drop (7-Zip and WinMerge register here too).
const wchar_t* kDragDropKeys[] = {
    L"Directory\\shellex\\DragDropHandlers\\AngelCOPY",
    L"Drive\\shellex\\DragDropHandlers\\AngelCOPY",
};

const wchar_t* kCtxMenuKeys[] = {
    L"*\\shellex\\ContextMenuHandlers\\AngelCOPY",
    L"Directory\\shellex\\ContextMenuHandlers\\AngelCOPY",
    L"Directory\\Background\\shellex\\ContextMenuHandlers\\AngelCOPY",
};

// Legacy keys written by AngelCOPY <= 1.0 (the DropHandler that never ran).
const wchar_t* kLegacyDropKeys[] = {
    L"Directory\\shellex\\DropHandler",
    L"Directory\\Background\\shellex\\DropHandler",
    L"Drive\\shellex\\DropHandler",
};
const wchar_t* kLegacyBackupKey = L"Software\\AngelCOPY";
const wchar_t* kLegacyBackupNames[] = {L"Directory", L"Directory.Background", L"Drive"};

LONG WriteString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                 const std::wstring& data) {
    HKEY h;
    LONG r = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &h,
                             nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(h, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(data.c_str()),
                       (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r;
}

bool ReadString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                std::wstring& out) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return false;
    wchar_t buf[512];
    DWORD cb = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_SZ) return false;
    out.assign(buf);
    return true;
}

bool ClsidExists(const std::wstring& clsid) {
    HKEY h;
    std::wstring k = L"CLSID\\" + clsid + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, k.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    return path;
}

void RegisterComObject(const std::wstring& clsidStr,
                       const std::wstring& friendlyName,
                       const std::wstring& dllPath) {
    std::wstring base = L"CLSID\\" + clsidStr;
    WriteString(HKEY_CLASSES_ROOT, base, nullptr, friendlyName);
    WriteString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", nullptr, dllPath);
    WriteString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", L"ThreadingModel",
                L"Apartment");
}

// Undo the dead DropHandler registration from older versions. Windows ships no
// DropHandler for Directory/Drive at all, so the correct "restore" is usually to
// DELETE the key — the previous version instead wrote a hardcoded CLSID that
// does not even exist on the system, which would leave broken junk behind.
// Only put a value back if it names a class that really exists (i.e. some other
// product owned this key before us).
void CleanupLegacyDropHandler() {
    for (size_t i = 0; i < ARRAYSIZE(kLegacyDropKeys); ++i) {
        std::wstring current;
        if (!ReadString(HKEY_CLASSES_ROOT, kLegacyDropKeys[i], nullptr, current))
            continue;                       // key absent: nothing to undo
        if (_wcsicmp(current.c_str(), ANGEL_DROPHANDLER_CLSID_STR) != 0)
            continue;                       // not ours: leave it alone

        std::wstring backup;
        bool restore = ReadString(HKEY_LOCAL_MACHINE, kLegacyBackupKey,
                                  kLegacyBackupNames[i], backup) &&
                       !backup.empty() && ClsidExists(backup);
        if (restore)
            WriteString(HKEY_CLASSES_ROOT, kLegacyDropKeys[i], nullptr, backup);
        else
            SHDeleteKeyW(HKEY_CLASSES_ROOT, kLegacyDropKeys[i]);
    }
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, kLegacyBackupKey);
    SHDeleteKeyW(HKEY_CLASSES_ROOT,
                 (std::wstring(L"CLSID\\") + ANGEL_DROPHANDLER_CLSID_STR).c_str());
}

} // namespace

STDAPI DllRegisterServer() {
    std::wstring dll = ModulePath();

    // Remove the dead DropHandler from any previous install first.
    CleanupLegacyDropHandler();

    RegisterComObject(ANGEL_CONTEXTMENU_CLSID_STR, L"AngelCOPY Context Menu", dll);
    RegisterComObject(ANGEL_DRAGDROP_CLSID_STR, L"AngelCOPY Drag Drop Handler", dll);

    for (auto* key : kCtxMenuKeys)
        WriteString(HKEY_CLASSES_ROOT, key, nullptr, ANGEL_CONTEXTMENU_CLSID_STR);
    for (auto* key : kDragDropKeys)
        WriteString(HKEY_CLASSES_ROOT, key, nullptr, ANGEL_DRAGDROP_CLSID_STR);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    CleanupLegacyDropHandler();

    for (auto* key : kCtxMenuKeys) SHDeleteKeyW(HKEY_CLASSES_ROOT, key);
    for (auto* key : kDragDropKeys) SHDeleteKeyW(HKEY_CLASSES_ROOT, key);

    SHDeleteKeyW(HKEY_CLASSES_ROOT,
                 (std::wstring(L"CLSID\\") + ANGEL_CONTEXTMENU_CLSID_STR).c_str());
    SHDeleteKeyW(HKEY_CLASSES_ROOT,
                 (std::wstring(L"CLSID\\") + ANGEL_DRAGDROP_CLSID_STR).c_str());

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
