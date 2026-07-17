#pragma once
#include <string>
#include <unordered_set>
#include <vector>

namespace angelcopy {

enum class Operation { Copy, Move };

// What to do about destination files that already exist and differ.
enum class Conflict {
    Replace,        // robocopy default: overwrite anything that differs
    Skip,           // /XC /XN /XO -> only copy files not already there
    ReplaceIfNewer, // /XO -> never let an older source overwrite a newer dest
};

// One robocopy invocation: a source directory, a destination directory, and
// zero or more file filters. Empty `files` means "copy the whole directory
// tree" (folder mode); a non-empty list means "copy just these files out of
// srcDir into dstDir" (file mode, since robocopy is directory-oriented).
struct RoboJob {
    std::wstring srcDir;
    std::wstring dstDir;
    std::vector<std::wstring> files; // empty => whole-tree copy
};

// Turn (operation, destination, sources) into a minimal set of robocopy jobs.
// Directories each become their own whole-tree job; loose files are grouped by
// parent directory so same-folder files share a single robocopy call.
std::vector<RoboJob> PlanJobs(Operation op,
                              const std::wstring& destDir,
                              const std::vector<std::wstring>& sources);

// Build the robocopy.exe argument string (everything after the exe path) for a
// job. `parseable` selects the machine-readable flag set (/BYTES /FP /NC ...)
// the progress UI parses; false keeps the plain human console output.
std::wstring BuildRobocopyArgs(Operation op, const RoboJob& job, bool parseable,
                               Conflict policy);

// Location of robocopy.exe in the real System32.
std::wstring RobocopyExe();

// Pre-scan across all jobs: total bytes/files (drives the percentage bar) plus
// the destination files that already exist AND differ from the source — i.e.
// exactly the files robocopy would silently overwrite. Identical files are not
// conflicts: robocopy skips them and nothing is lost.
struct ScanResult {
    // Files bucketed the way robocopy will treat them. Identical files are
    // never copied, so they must not count toward the progress total.
    unsigned long long lonelyBytes = 0, lonelyFiles = 0; // not at destination
    unsigned long long sameBytes = 0,   sameFiles = 0;   // identical -> skipped
    unsigned long long newerBytes = 0,  newerFiles = 0;  // differs, src not older
    unsigned long long olderBytes = 0,  olderFiles = 0;  // differs, src older
    unsigned long long conflicts = 0;                    // newerFiles + olderFiles
    std::vector<std::wstring> conflictSample;            // capped, for the dialog

    // Lowercased SOURCE paths per class (lonely files are never skipped, so
    // they are not kept). The progress UI needs these to tell a skip line from
    // a copy line: with /V robocopy lists skipped files too, and under /NC the
    // two line kinds are textually identical — only the path says which is which.
    std::vector<std::wstring> samePaths, newerPaths, olderPaths;
};
ScanResult ScanJobs(const std::vector<RoboJob>& jobs);

// The exact set of (lowercased) source paths robocopy will list but NOT copy
// under `policy` — same files always, plus the policy-excluded classes.
std::unordered_set<std::wstring> SkipSetFor(const ScanResult& s, Conflict policy);

// Mirror support: everything at the DESTINATION that does not exist in the
// source — what a mirror must delete. Only whole-tree jobs are examined
// (mirroring is a folder operation; loose-file jobs are plain copies and never
// purge anything). Entries whose type differs (file here, folder there) count
// as extras too, so the copy phase can recreate them cleanly. Reparse points
// are returned as targets but never recursed into — the deleter removes the
// link itself, not what it points at. Returned paths are top-level extras;
// count/measure them with ScanDelete before prompting.
std::vector<std::wstring> ScanExtras(const std::vector<RoboJob>& jobs);

// Bytes/files robocopy will actually copy under `policy` — this is what the
// progress bar must be scaled against, otherwise it never reaches 100%.
void ExpectedFor(const ScanResult& s, Conflict policy,
                 unsigned long long& bytes, unsigned long long& files);

// What will NOT be copied, split by reason, so the UI can report it.
struct SkipInfo {
    unsigned long long identicalFiles = 0, identicalBytes = 0; // already up to date
    unsigned long long policyFiles = 0,    policyBytes = 0;    // excluded by policy
    bool any() const { return identicalFiles || policyFiles; }
};
SkipInfo SkippedFor(const ScanResult& s, Conflict policy);

// Run every job sequentially, printing to the console. Returns the worst
// robocopy exit code seen (robocopy: 0-7 = success bit-flags, >=8 = failure).
int RunJobs(Operation op, const std::vector<RoboJob>& jobs, Conflict policy);

} // namespace angelcopy
