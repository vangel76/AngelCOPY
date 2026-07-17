#include "Common.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <string>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

HMODULE g_hModule = nullptr;
long    g_cDllRef = 0;

namespace angel {

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring RunnerPath() { return ModuleDir() + L"\\AngelCopyRunner.exe"; }

wchar_t DriveLetter(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        wchar_t c = path[0];
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        return c;
    }
    return 0;
}

// ---- CF_HDROP extraction from an IDataObject ----
bool GetHDropPaths(IDataObject* pdo, std::vector<std::wstring>& out) {
    if (!pdo) return false;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (FAILED(pdo->GetData(&fmt, &stg))) return false;

    bool ok = false;
    HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
    if (hDrop) {
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            std::wstring buf(len + 1, L'\0');
            DragQueryFileW(hDrop, i, &buf[0], len + 1);
            buf.resize(len);
            if (!buf.empty()) out.push_back(buf);
        }
        ok = count > 0;
        GlobalUnlock(stg.hGlobal);
    }
    ReleaseStgMedium(&stg);
    return ok;
}

DWORD GetPreferredDropEffect(IDataObject* pdo) {
    if (!pdo) return 0;
    static UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    FORMATETC fmt{(CLIPFORMAT)cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (FAILED(pdo->GetData(&fmt, &stg))) return 0;
    DWORD effect = 0;
    void* p = GlobalLock(stg.hGlobal);
    if (p) {
        effect = *reinterpret_cast<DWORD*>(p);
        GlobalUnlock(stg.hGlobal);
    }
    ReleaseStgMedium(&stg);
    return effect;
}

// ---- Clipboard ----
bool ClipboardHasHDrop() { return IsClipboardFormatAvailable(CF_HDROP) != FALSE; }

bool GetClipboardHDrop(std::vector<std::wstring>& out, DWORD& effect) {
    effect = 0;
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) {
        HDROP hDrop = static_cast<HDROP>(GlobalLock(h));
        if (hDrop) {
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                std::wstring buf(len + 1, L'\0');
                DragQueryFileW(hDrop, i, &buf[0], len + 1);
                buf.resize(len);
                if (!buf.empty()) out.push_back(buf);
            }
            ok = count > 0;
            GlobalUnlock(h);
        }
    }
    UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HANDLE he = GetClipboardData(cf);
    if (he) {
        void* p = GlobalLock(he);
        if (p) { effect = *reinterpret_cast<DWORD*>(p); GlobalUnlock(he); }
    }
    CloseClipboard();
    return ok;
}

// ---- Runner launch ----
static std::wstring WriteTempList(const std::vector<std::wstring>& sources) {
    wchar_t tmpDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmpDir)) return L"";
    wchar_t tmpFile[MAX_PATH]{};
    if (!GetTempFileNameW(tmpDir, L"acp", 0, tmpFile)) return L"";

    HANDLE h = CreateFileW(tmpFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";

    DWORD written = 0;
    const wchar_t bom = 0xFEFF;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    for (const auto& s : sources) {
        WriteFile(h, s.c_str(), (DWORD)(s.size() * sizeof(wchar_t)), &written, nullptr);
        const wchar_t nl = L'\n';
        WriteFile(h, &nl, sizeof(nl), &written, nullptr);
    }
    CloseHandle(h);
    return tmpFile;
}

bool LaunchRunner(const wchar_t* op, const std::wstring& dest,
                  const std::vector<std::wstring>& sources) {
    const bool isDelete = (_wcsicmp(op, L"delete") == 0);
    if (sources.empty()) return false;
    if (!isDelete && dest.empty()) return false;

    std::wstring runner = RunnerPath();
    if (!PathFileExistsW(runner.c_str())) return false;

    std::wstring list = WriteTempList(sources);
    if (list.empty()) return false;

    // "<runner>" <copy|move> "<dest>" "@<list>"   /   "<runner>" delete "@<list>"
    std::wstring cmd = L"\"" + runner + L"\" ";
    cmd += op;
    cmd += L" ";
    if (!isDelete) cmd += L"\"" + dest + L"\" ";
    cmd += L"\"@" + list + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    // CREATE_NO_WINDOW: no console flash — the runner shows its own dialog.
    BOOL ok = CreateProcessW(runner.c_str(), mutableCmd.data(), nullptr, nullptr,
                             FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
}

} // namespace angel
