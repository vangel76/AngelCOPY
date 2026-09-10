#pragma once
#include <windows.h>
#include <cwctype>
#include <string>

// Header-only helpers shared by the DLL, the runner and the agent. Header-only
// on purpose: the unit tests compile single runner .cpp files against this
// without any build-script or link change, and the DLL gains no dependency.
namespace acutil {

// THE canonical \\?\ long-path form (CLAUDE.md load-bearing invariant: every
// walk must use it for enumeration AND stat, not just I/O). Already-prefixed
// paths pass through; UNC becomes \\?\UNC\server\share. Per-file wrappers
// (Ext/ExtPath/ExtP) forward here so the rule lives in exactly one place —
// the copies had already drifted once (the agent's inline version lacked the
// already-prefixed check).
inline std::wstring ExtLongPath(const std::wstring& p) {
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

// "98 KB" / "1.5 GB": one decimal below 10 units, none above.
inline std::wstring HumanBytes(unsigned long long b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t out[64];
    swprintf_s(out, 64, (v < 10 && i > 0) ? L"%.1f %s" : L"%.0f %s", v, u[i]);
    return out;
}

// Always two decimals (except plain bytes, which are exact): "11.23 TB".
// The properties dialog uses this — "11 TB" hides ~250 GB of rounding.
inline std::wstring HumanBytes2(unsigned long long b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t out[64];
    swprintf_s(out, 64, i > 0 ? L"%.2f %s" : L"%.0f %s", v, u[i]);
    return out;
}

// Lowercased copy for case-insensitive path keys. SkipSetFor matching and
// the exclusion set both depend on this exact transform — keep it single.
inline std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// The OS's localized message for a Win32 error, trailing CR/LF/space trimmed.
inline std::wstring WinErrText(DWORD err) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out = (n && msg) ? std::wstring(msg, n) : L"";
    if (msg) LocalFree(msg);
    while (!out.empty() &&
           (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
        out.pop_back();
    return out;
}

} // namespace acutil
