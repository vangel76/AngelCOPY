// AngelCopyRunner.exe
//
// The worker process AngelCOPY spawns to perform a transfer with
// robocopy /MT:64. Kept separate from the shell-extension DLL so a hang or
// crash here never destabilizes Explorer.
//
// Usage:
//   AngelCopyRunner.exe [--console] <copy|move|sync> <destDir> <src1> [src2 ...]
//   AngelCopyRunner.exe [--console] <copy|move|sync> <destDir> @<listFile>
//   AngelCopyRunner.exe [--console] delete <src...|@listFile>
//
// sync = mirror: copy source folders onto the destination, then permanently
// delete everything under the mirrored trees that does not exist in the
// source. Prompts first (GUI) / requires ANGELCOPY_CONFIRM_DELETE=1 (console).
//
// Default: shows a native progress dialog (percentage, speed, ETA, cancel).
// --console: plain text output, no window (used by automated tests).
//
// <listFile> is a UTF-16LE (BOM) text file, one source path per line. The DLL
// writes one to avoid the command-line length limit when many items are moved.

#include "Robocopy.h"
#include "ProgressUI.h"
#include "ConflictUI.h"
#include "ConfirmUI.h"
#include "Delete.h"

#include <windows.h>
#include <conio.h>
#include <string>
#include <vector>
#include <cstdio>

using namespace angelcopy;

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Read a UTF-16LE source list (one path per line). Returns false on I/O error.
bool ReadListFile(const std::wstring& path, std::vector<std::wstring>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::wstring text;
    char buf[4096];
    DWORD read = 0;
    std::string raw;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0)
        raw.append(buf, read);
    CloseHandle(h);

    // Interpret as UTF-16LE; skip a leading BOM if present.
    const wchar_t* w = reinterpret_cast<const wchar_t*>(raw.data());
    size_t wlen = raw.size() / sizeof(wchar_t);
    size_t start = (wlen && w[0] == 0xFEFF) ? 1 : 0;
    text.assign(w + start, wlen - start);

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line =
            Trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos
                                                           : nl - pos));
        if (!line.empty()) out.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return true;
}

int Usage() {
    fwprintf(stderr,
             L"Usage: AngelCopyRunner [--console] <copy|move|sync> <destDir> "
             L"<src...|@listfile>\n"
             L"       AngelCopyRunner [--console] delete <src...|@listfile>\n");
    return 2;
}

// Collect sources from either an @listfile or the remaining argv entries.
bool CollectSources(int argc, wchar_t** argv, int first,
                    std::vector<std::wstring>& sources) {
    if (argv[first][0] == L'@') {
        const wchar_t* listPath = argv[first] + 1;
        if (!ReadListFile(listPath, sources)) {
            fwprintf(stderr, L"[AngelCOPY] cannot read list file: %s\n", listPath);
            return false;
        }
        DeleteFileW(listPath); // consume the temp list written by the shell ext
    } else {
        for (int i = first; i < argc; ++i) sources.push_back(argv[i]);
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Optional leading --console flag selects headless text mode.
    bool consoleMode = false;
    int base = 1;
    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0) {
        consoleMode = true;
        base = 2;
    }
    if (argc - base < 2) return Usage();

    // ---- delete: no destination argument ----
    if (_wcsicmp(argv[base], L"delete") == 0) {
        std::vector<std::wstring> targets;
        if (!CollectSources(argc, argv, base + 1, targets)) return 3;
        if (targets.empty()) {
            fwprintf(stderr, L"[AngelCOPY] no items to delete\n");
            return 3;
        }

        DeleteScan scan = ScanDelete(targets);

        if (consoleMode) {
            // Headless: no prompt is possible, and this is irreversible — so
            // require an explicit opt-in rather than silently deleting.
            if (GetEnvironmentVariableW(L"ANGELCOPY_CONFIRM_DELETE", nullptr, 0) == 0) {
                fwprintf(stderr,
                         L"[AngelCOPY] refusing to delete %llu files without a "
                         L"prompt.\n           Set ANGELCOPY_CONFIRM_DELETE=1 to "
                         L"confirm in --console mode.\n",
                         scan.files);
                return 3;
            }
            DeleteSink sink;
            sink.onError = [](const std::wstring& m) {
                fwprintf(stderr, L"[AngelCOPY] %s\n", m.c_str());
            };
            int rc = DeleteTargets(targets, sink);
            return rc >= 8 ? rc : 0;
        }

        if (!AskDeleteConfirm(scan)) return 0; // nothing touched
        int code = RunDeleteWithUI(targets, scan.bytes, scan.files);
        return (code >= 8) ? code : 0;
    }

    // ---- copy / move ----
    if (argc - base < 3) return Usage();

    Operation op;
    bool sync = false;
    if (_wcsicmp(argv[base], L"copy") == 0)
        op = Operation::Copy;
    else if (_wcsicmp(argv[base], L"move") == 0)
        op = Operation::Move;
    else if (_wcsicmp(argv[base], L"sync") == 0) {
        op = Operation::Copy; // mirror = copy phase + purge phase
        sync = true;
    } else
        return Usage();

    std::wstring dest = argv[base + 1];

    std::vector<std::wstring> sources;
    if (!CollectSources(argc, argv, base + 2, sources)) return 3;

    if (sources.empty()) {
        fwprintf(stderr, L"[AngelCOPY] no source items\n");
        return 3;
    }

    std::vector<RoboJob> jobs = PlanJobs(op, dest, sources);

    // ---- sync (mirror): confirmed copy + purge ----
    if (sync) {
        ScanResult scan = ScanJobs(jobs);
        unsigned long long bytes = 0, files = 0;
        // A mirror overwrites whatever differs — Replace, no conflict prompt;
        // the confirmation below states it instead.
        ExpectedFor(scan, Conflict::Replace, bytes, files);
        SkipInfo skipped = SkippedFor(scan, Conflict::Replace);
        std::unordered_set<std::wstring> skipSet =
            SkipSetFor(scan, Conflict::Replace);
        std::vector<std::wstring> extras = ScanExtras(jobs);
        DeleteScan delScan = ScanDelete(extras);

        if (consoleMode) {
            // Headless mirror deletes without a prompt — same explicit opt-in
            // as headless delete.
            if (!extras.empty() &&
                GetEnvironmentVariableW(L"ANGELCOPY_CONFIRM_DELETE", nullptr, 0) == 0) {
                fwprintf(stderr,
                         L"[AngelCOPY] mirror would delete %llu items at the "
                         L"destination; refusing without a prompt.\n           Set "
                         L"ANGELCOPY_CONFIRM_DELETE=1 to confirm in --console mode.\n",
                         delScan.files + delScan.dirs);
                return 3;
            }
            int worst = RunJobs(op, jobs, Conflict::Replace);
            if (worst < 8) {
                DeleteSink sink;
                sink.onError = [](const std::wstring& m) {
                    fwprintf(stderr, L"[AngelCOPY] %s\n", m.c_str());
                };
                int rd = DeleteTargets(extras, sink);
                if (rd > worst) worst = rd;
            }
            return worst >= 8 ? worst : 0;
        }

        if (!AskSyncConfirm(files, bytes, delScan)) return 0; // nothing touched
        int code = RunSyncWithUI(jobs, bytes, files, skipped, skipSet, extras);
        return (code >= 8) ? code : 0;
    }

    if (consoleMode) {
        // Headless: no prompt possible, so use robocopy's default behavior.
        int worst = RunJobs(op, jobs, Conflict::Replace);
        if (worst >= 8) {
            fwprintf(stderr,
                     L"\n[AngelCOPY] finished with errors (robocopy code %d).\n",
                     worst);
            return worst;
        }
        return 0;
    }

    // GUI mode: pre-scan classifies every file against the destination.
    ScanResult scan = ScanJobs(jobs);

    // Only prompt when files would actually be overwritten. Identical files are
    // skipped by robocopy and are not conflicts.
    Conflict policy = Conflict::Replace;
    if (scan.conflicts > 0) {
        bool cancelled = false;
        policy = AskConflict(op, scan.conflicts, scan.conflictSample, cancelled);
        if (cancelled) return 0; // user aborted before anything was touched
    }

    unsigned long long bytes = 0, files = 0;
    ExpectedFor(scan, policy, bytes, files);
    SkipInfo skipped = SkippedFor(scan, policy);
    // Source paths robocopy will list (/V) but not copy — lets the dialog
    // paint those stretches green instead of counting them as speed.
    std::unordered_set<std::wstring> skipSet = SkipSetFor(scan, policy);

    int code = RunJobsWithUI(op, dest, jobs, bytes, files, policy, skipped, skipSet);
    return (code >= 8) ? code : 0;
}
