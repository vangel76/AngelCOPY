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
    DWORD n = GetModuleFileNameW(g_hModule, path, MAX_PATH);
    // n == MAX_PATH means the name was truncated and may not be NUL-terminated
    // (and ERROR_INSUFFICIENT_BUFFER is set). Treat as failure rather than read
    // an unterminated buffer. A Program Files install never hits this.
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring RunnerPath() { return ModuleDir() + L"\\AngelCopyRunner.exe"; }

// ---- CF_HDROP extraction ----

// The single place the hostile-HGLOBAL guard lives: any drag source or
// clipboard writer can hand over a block too small for a DROPFILES header,
// and DragQueryFileW trusts the header's pFiles offset — an undersized block
// walks past the allocation, inside explorer.exe where this DLL lives.
static bool ReadHDropGlobal(HANDLE h, std::vector<std::wstring>& out) {
    if (!h || GlobalSize(h) < sizeof(DROPFILES)) return false;
    bool ok = false;
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
    return ok;
}

// Same shape for the 4-byte effect DWORD: a crafted block smaller than a
// DWORD must not be read.
static DWORD ReadEffectGlobal(HANDLE h) {
    if (!h) return 0;
    DWORD effect = 0;
    void* p = GlobalLock(h);
    if (p && GlobalSize(h) >= sizeof(DWORD))
        effect = *reinterpret_cast<DWORD*>(p);
    if (p) GlobalUnlock(h);
    return effect;
}

bool GetHDropPaths(IDataObject* pdo, std::vector<std::wstring>& out) {
    if (!pdo) return false;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (FAILED(pdo->GetData(&fmt, &stg))) return false;
    bool ok = (stg.tymed == TYMED_HGLOBAL) && ReadHDropGlobal(stg.hGlobal, out);
    ReleaseStgMedium(&stg);
    return ok;
}

bool GetClipboardHDrop(std::vector<std::wstring>& out, DWORD& effect) {
    effect = 0;
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = ReadHDropGlobal(GetClipboardData(CF_HDROP), out);
    UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    effect = ReadEffectGlobal(GetClipboardData(cf));
    CloseClipboard();
    return ok;
}

// ---- Runner launch ----

// Quote one command-line argument. Windows filenames cannot contain a double
// quote, so the only hazard is a path ending in backslash(es): "C:\" makes the
// closing quote escaped (\") and merges the next argument in. Double the run of
// trailing backslashes so the terminator survives (the canonical MSVCRT rule).
static std::wstring QuoteArg(const std::wstring& s) {
    size_t tail = 0;
    while (tail < s.size() && s[s.size() - 1 - tail] == L'\\') ++tail;
    return L"\"" + s + std::wstring(tail, L'\\') + L"\"";
}

// Returns true only if the whole buffer was written (a short write on a full
// disk would truncate the source list and silently drop items).
static bool WriteAll(HANDLE h, const void* data, DWORD bytes) {
    const BYTE* p = static_cast<const BYTE*>(data);
    while (bytes) {
        DWORD n = 0;
        if (!WriteFile(h, p, bytes, &n, nullptr) || n == 0) return false;
        p += n;
        bytes -= n;
    }
    return true;
}

static std::wstring WriteTempList(const std::vector<std::wstring>& sources) {
    wchar_t tmpDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmpDir)) return L"";
    wchar_t tmpFile[MAX_PATH]{};
    if (!GetTempFileNameW(tmpDir, L"acp", 0, tmpFile)) return L"";

    HANDLE h = CreateFileW(tmpFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    // GetTempFileNameW already CREATED the file, so a failed open must clean it
    // up or a 0-byte acpXXXX.tmp is left behind on every failure.
    if (h == INVALID_HANDLE_VALUE) { DeleteFileW(tmpFile); return L""; }

    bool ok = true;
    const wchar_t bom = 0xFEFF;
    ok = WriteAll(h, &bom, sizeof(bom));
    for (const auto& s : sources) {
        if (!ok) break;
        ok = WriteAll(h, s.c_str(), (DWORD)(s.size() * sizeof(wchar_t))) &&
             WriteAll(h, L"\n", sizeof(wchar_t));
    }
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmpFile); return L""; } // truncated -> don't use it
    return tmpFile;
}

bool LaunchRunner(const wchar_t* op, const std::wstring& dest,
                  const std::vector<std::wstring>& sources) {
    // delete and props take no destination argument.
    const bool noDest = (_wcsicmp(op, L"delete") == 0) ||
                        (_wcsicmp(op, L"props") == 0);
    if (sources.empty()) return false;
    if (!noDest && dest.empty()) return false;

    std::wstring runner = RunnerPath();
    if (!PathFileExistsW(runner.c_str())) return false;

    std::wstring list = WriteTempList(sources);
    if (list.empty()) return false;

    // "<runner>" <copy|move> "<dest>" "@<list>"   /   "<runner>" delete "@<list>"
    // Every path is quoted with QuoteArg so a drive-root target ("D:\") can't
    // escape its quote and swallow the @list argument.
    std::wstring cmd = QuoteArg(runner) + L" ";
    cmd += op;
    cmd += L" ";
    if (!noDest) cmd += QuoteArg(dest) + L" ";
    cmd += QuoteArg(L"@" + list);

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
    } else {
        // The runner consumes+deletes the list; if it never starts, we must.
        DeleteFileW(list.c_str());
    }
    return ok != FALSE;
}

} // namespace angel
