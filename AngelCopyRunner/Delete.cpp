#include "Delete.h"

#include <windows.h>

namespace angelcopy {

namespace {

std::wstring StripSep(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// \\?\ form so paths longer than MAX_PATH work. Explorer refuses those; we
// should not inherit that limit when deleting.
std::wstring Ext(const std::wstring& p) {
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + p.substr(2);   // \\server\share -> \\?\UNC\server\share
    return L"\\\\?\\" + p;
}

std::wstring BaseName(const std::wstring& p) {
    std::wstring s = StripSep(p);
    size_t i = s.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? s : s.substr(i + 1);
}

std::wstring SysError(DWORD e) {
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = (n && buf) ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

void ReportError(const DeleteSink& sink, const std::wstring& path, DWORD e) {
    if (sink.onError)
        sink.onError(path + L"  \x2014  " + SysError(e));
}

bool Cancelled(const DeleteSink& sink) {
    return sink.cancelled && sink.cancelled();
}

bool DeleteOneFile(const std::wstring& path, unsigned long long size,
                   const DeleteSink& sink) {
    std::wstring x = Ext(path);
    if (DeleteFileW(x.c_str())) {
        if (sink.onFile) sink.onFile(path, size);
        return true;
    }
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
        // Most likely read-only; Explorer prompts here, we just clear it.
        SetFileAttributesW(x.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(x.c_str())) {
            if (sink.onFile) sink.onFile(path, size);
            return true;
        }
        e = GetLastError();
    }
    ReportError(sink, path, e);
    return false;
}

bool RemoveDir(const std::wstring& dir, const DeleteSink& sink) {
    std::wstring x = Ext(dir);
    if (RemoveDirectoryW(x.c_str())) return true;
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(x.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(x.c_str())) return true;
        e = GetLastError();
    }
    ReportError(sink, dir, e);
    return false;
}

// Returns false if anything under `dir` could not be removed.
bool DeleteTree(const std::wstring& dir, const DeleteSink& sink) {
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((Ext(dir) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) { ReportError(sink, dir, e); return false; }
    } else {
        do {
            if (Cancelled(sink)) { FindClose(h); return false; }
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring child = dir + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    // Junction/symlink: remove the link, never follow it —
                    // following would delete the target's contents.
                    if (!RemoveDir(child, sink)) ok = false;
                } else {
                    if (!DeleteTree(child, sink)) ok = false;
                }
            } else {
                unsigned long long size =
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (!DeleteOneFile(child, size, sink)) ok = false;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (Cancelled(sink)) return false;
    if (ok && !RemoveDir(dir, sink)) ok = false;
    return ok;
}

void ScanTree(const std::wstring& dir, DeleteScan& acc) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((Ext(dir) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            acc.dirs += 1;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ScanTree(child, acc);
        } else {
            acc.files += 1;
            acc.bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

DeleteScan ScanDelete(const std::vector<std::wstring>& targets) {
    DeleteScan s;
    for (const auto& raw : targets) {
        std::wstring t = StripSep(raw);
        if (t.empty()) continue;
        s.sample.push_back(BaseName(t));

        DWORD attr = GetFileAttributesW(Ext(t).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            s.dirs += 1;
            if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) ScanTree(t, s);
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(Ext(t).c_str(), GetFileExInfoStandard, &fa)) {
                s.files += 1;
                s.bytes += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            }
        }
    }
    return s;
}

int DeleteTargets(const std::vector<std::wstring>& targets, const DeleteSink& sink) {
    bool ok = true;
    for (const auto& raw : targets) {
        if (Cancelled(sink)) break;
        std::wstring t = StripSep(raw);
        if (t.empty()) continue;

        DWORD attr = GetFileAttributesW(Ext(t).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            ReportError(sink, t, GetLastError());
            ok = false;
            continue;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!RemoveDir(t, sink)) ok = false;
            } else if (!DeleteTree(t, sink)) {
                ok = false;
            }
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            unsigned long long size = 0;
            if (GetFileAttributesExW(Ext(t).c_str(), GetFileExInfoStandard, &fa))
                size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (!DeleteOneFile(t, size, sink)) ok = false;
        }
    }
    return ok ? 0 : 8;
}

} // namespace angelcopy
