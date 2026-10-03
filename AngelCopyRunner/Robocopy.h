#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace angelcopy {

// Live feedback + cancellation for the pre-transfer scans. On a 500k-file
// tree the destination comparison takes seconds; the GUI shows a "Preparing"
// window (RunScanWithUI) that reads `files` while the scan runs on a worker
// thread. Setting `cancel` aborts the walk early — the partial result must
// then be discarded by the caller.
struct ScanProgress {
    std::atomic<unsigned long long> files{0};
    // Live dirs/bytes are fed by the delete/properties count (ScanDelete,
    // ScanAllocated); the copy scans only tick `files`. All monotonic.
    std::atomic<unsigned long long> dirs{0};
    std::atomic<unsigned long long> bytes{0};
    std::atomic<long> cancel{0};
};

enum class Operation { Copy, Move };

// What to do about destination files that already exist and differ.
enum class Conflict {
    Replace,        // robocopy default: overwrite anything that differs
    Skip,           // /XC /XN /XO -> only copy files not already there
    ReplaceIfNewer, // /XO -> never let an older source overwrite a newer dest
};

// One copy job: a source directory, a destination directory, and zero or
// more file filters. Empty `files` means "copy the whole directory tree"
// (folder mode); a non-empty list means "copy just these files out of srcDir
// into dstDir". (Named RoboJob for historic reasons — the planning layer was
// built for robocopy before the native engine replaced it entirely.)
struct RoboJob {
    std::wstring srcDir;
    std::wstring dstDir;
    std::vector<std::wstring> files; // empty => whole-tree copy
    // Parallel to `files`: destination filename for each, when it must differ
    // from the source name (same-folder copy → "x - Kopie"). Empty vector =>
    // every file keeps its name. Whole-tree self-copy is handled by renaming
    // dstDir instead, so this stays empty there.
    std::vector<std::wstring> dstNames;
};

// Turn (operation, destination, sources) into a minimal set of robocopy jobs.
// Directories each become their own whole-tree job; loose files are grouped by
// parent directory so same-folder files share a single robocopy call.
// `copyWord` is the localized word for a same-folder copy ("Kopie"/"Copy"):
// pasting an item into the folder it already lives in produces
// "<name> - <copyWord>" instead of colliding with itself (Explorer behavior).
// Passed in (not looked up) so Robocopy.cpp stays free of the Localize
// dependency the unit tests don't link.
std::vector<RoboJob> PlanJobs(Operation op,
                              const std::wstring& destDir,
                              const std::vector<std::wstring>& sources,
                              const std::wstring& copyWord = L"Copy");

// How the engine will treat a source file given what's at the destination.
// Public because the native engine (NativeCopy.cpp) must make the exact same
// per-file decision the scan made, or totals and outcomes drift apart.
enum class FileClass { Lonely, Same, DiffNewer, DiffOlder };

// One destination file that exists AND differs — what the per-image compare
// dialog shows and what the per-file decision totals are corrected from.
struct ConflictItem {
    std::wstring src, dst;
    unsigned long long size = 0, dstSize = 0;
    FileClass fc = FileClass::DiffNewer;
};

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
    // Net disk space an overwrite of these classes ADDS: sum of
    // max(0, srcSize - dstSize). robocopy overwrites in-place (measured: a 2 GB
    // source over a 500 MB destination consumes only ~1.5 GB), so the space a
    // copy needs is the growth, not the full source size. Shrinking files add
    // nothing (and never count as freed — /MT ordering is unknown).
    unsigned long long newerGrowBytes = 0, olderGrowBytes = 0;
    unsigned long long conflicts = 0;                    // newerFiles + olderFiles
    std::vector<std::wstring> conflictSample;            // capped, for the dialog

    // Per-file verdicts (lowercased source path -> class), collected only when
    // SetCollectClasses is on. Handed to the engine via SetCarriedClasses so
    // the copy phase skips its second round of destination stats — on a
    // skip-heavy mirror over a slow target those re-stats WERE the copy
    // phase. Fast-path Lonely files (destination dir absent) are not in here;
    // the engine's dstFresh shortcut covers them statlessly anyway.
    std::unordered_map<std::wstring, FileClass> classes;

    // EVERY conflict with its sizes, collected only when SetCollectConflicts
    // is on and only up to kMaxConflictItems — the per-file compare dialog
    // needs the complete list (conflictItemsComplete()), or it is not offered.
    std::vector<ConflictItem> conflictItems;
    bool conflictItemsComplete() const {
        return conflictItems.size() == conflicts;
    }
};
ScanResult ScanJobs(const std::vector<RoboJob>& jobs,
                    ScanProgress* prog = nullptr);

// Mirrors robocopy's own comparison: identical == same size and write-time
// within 2s (FAT/network timestamp granularity). Fills `dstSize` (0 when the
// destination file is absent) so the caller can compute overwrite growth.
FileClass ClassifyFile(const std::wstring& dst, unsigned long long srcSize,
                       const FILETIME& srcTime, unsigned long long& dstSize);

// THE policy predicate: does `policy` copy a file of class `fc`? The native
// engine's per-file decision AND every aggregate (ExpectedFor, SkippedFor,
// NeededSpaceFor's growth terms) derive from this one definition. It used to
// live as hand-mirrored switch tables kept aligned only by comments — the
// drift-prone shape that produced the scan/engine divergence bugs. Verified
// end-to-end by tests\test_conflict.cpp.
bool PolicyCopies(Conflict policy, FileClass fc);

// Mirror support: everything at the DESTINATION that does not exist in the
// source — what a mirror must delete. Only whole-tree jobs are examined
// (mirroring is a folder operation; loose-file jobs are plain copies and never
// purge anything). Entries whose type differs (file here, folder there) count
// as extras too, so the copy phase can recreate them cleanly. Reparse points
// are returned as targets but never recursed into — the deleter removes the
// link itself, not what it points at. Returned paths are top-level extras;
// count/measure them with ScanDelete before prompting.
std::vector<std::wstring> ScanExtras(const std::vector<RoboJob>& jobs,
                                     ScanProgress* prog = nullptr);

// Bytes/files robocopy will actually copy under `policy` — this is what the
// progress bar must be scaled against, otherwise it never reaches 100%.
void ExpectedFor(const ScanResult& s, Conflict policy,
                 unsigned long long& bytes, unsigned long long& files);

// Disk space the copy will actually consume at the destination under `policy`.
// NOT the same as ExpectedFor's byte count: lonely files cost their full size,
// but overwrites cost only their growth (robocopy writes in-place). Compare
// this against the destination volume's free space to warn before starting.
unsigned long long NeededSpaceFor(const ScanResult& s, Conflict policy);

// What will NOT be copied, split by reason, so the UI can report it.
struct SkipInfo {
    unsigned long long identicalFiles = 0, identicalBytes = 0; // already up to date
    unsigned long long policyFiles = 0,    policyBytes = 0;    // excluded by policy
    // Per-file decisions were made (compare dialog): the policy skips are
    // then the user's own picks, and the report says so instead of naming a
    // policy button that was never pressed.
    bool byChoice = false;
    bool any() const { return identicalFiles || policyFiles; }
};
SkipInfo SkippedFor(const ScanResult& s, Conflict policy);

// ---- per-file conflict decisions (the image compare dialog) --------------
// What the user picked for ONE conflicting file. Overrides the policy for
// that file only; every file without a decision follows the policy.
enum class FileAction { Overwrite, Skip, Rename };
struct FileDecision {
    FileAction action = FileAction::Overwrite;
    std::wstring newName; // Rename only: the new FILE name ("a (2).jpg"), same dir
};

// Same lifecycle as the carried classes: set once between the prompt and the
// run, read concurrently by the copy pool. Keyed by the LOWERCASED source
// path. The engine consults it only for files that classify as a conflict
// (DiffNewer/DiffOlder) at copy time — a file that became identical or
// vanished at the destination since the prompt needs no decision.
//
// Never set for a mirror: a renamed "a (2).jpg" is not in the source, so the
// purge phase would delete the very file the user asked to keep.
void SetFileDecisions(std::unordered_map<std::wstring, FileDecision>&& m);
bool AnyFileDecisions();
bool LookupFileDecision(const std::wstring& srcLower, FileDecision& d);

// Collect ScanResult::conflictItems (default OFF; GUI copy/move turn it on).
constexpr size_t kMaxConflictItems = 10000;
void SetCollectConflicts(bool on);

// Correct the policy totals (ExpectedFor / SkippedFor / NeededSpaceFor) for
// the decisions currently set: a file the policy copies but the user skipped
// moves into the skip line and vice versa; a rename costs its FULL size on
// disk (a new file), an overwrite only its growth (in-place, measured).
void AdjustForDecisions(const std::vector<ConflictItem>& items, Conflict policy,
                        unsigned long long& bytes, unsigned long long& files,
                        SkipInfo& skipped, unsigned long long& need);

// Image by extension (photo + RAW formats) — decides which conflicts the
// compare dialog walks. Everything else goes through the all-at-once prompt.
bool IsImageFile(const std::wstring& path);

// "<stem> (n)<ext>" — Explorer's keep-both naming ("IMG_1.jpg" -> "IMG_1 (2).jpg").
std::wstring NumberedName(const std::wstring& name, int n);

// Hands out "keep both" names that collide with nothing: not an existing
// destination entry, not a file still COMING IN to that destination folder
// (source "a (2).jpg" next to a renamed "a.jpg" would otherwise be overwritten
// by it, or overwrite it), and not a name already handed out this run.
class RenamePlanner {
public:
    explicit RenamePlanner(const std::vector<RoboJob>& jobs);
    // The name Reserve would hand out right now, without reserving it (the
    // compare dialog shows it before the user decides).
    std::wstring Peek(const ConflictItem& it) const;
    // Picks and reserves the new file name for a conflicting item.
    std::wstring Reserve(const ConflictItem& it);
private:
    std::unordered_set<std::wstring> taken_; // lowercased full dst paths
    // lowercased loose-job dstDir -> lowercased incoming file names
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> incoming_;
};

// ---- directory exclusions (the Unreal preset) ----------------------------
// A set of directory NAMES (matched at any depth, case-insensitive) skipped
// entirely — copy AND mirror-purge, exactly like robocopy /XD. This is
// process-wide state set ONCE in main before any scan/transfer and read
// read-only by every walk thereafter (both engines), so it isn't threaded
// through every walker signature. Empty by default -> nothing excluded, which
// is what the unit tests get.
//
// Load-bearing: an excluded dir must be invisible to BOTH the copy walk and
// the extras/purge walk. If the purge saw it (present at the destination,
// "not in source because we skipped it") it would DELETE the very cache the
// user asked to keep — the same data-loss shape as the source-junction bug.
void SetExcludedDirs(const std::vector<std::wstring>& names); // lowercased inside
bool IsExcludedDir(const std::wstring& name);
bool IsExcludedDir(const wchar_t* name); // alloc-free early-out for fd.cFileName
bool AnyExcludedDirs();

// Carried classification: the scan's per-file verdicts, handed to the native
// engine so the copy phase consumes them instead of re-statting every
// destination file (the second stat round doubled "slow target" mirrors).
// Process-wide like the exclusion set: set once between scan and run, read
// concurrently by the copy pool. A map larger than kMaxCarriedClasses is
// DISCARDED (memory cap; the engine then re-classifies as before). A lookup
// miss (file appeared/changed since the scan) also falls back to a real
// ClassifyFile — carried verdicts are an optimization, never the only truth.
constexpr size_t kMaxCarriedClasses = 400000; // ~100 MB worst case
void SetCollectClasses(bool on); // scan-side collection (default: off)
void SetCarriedClasses(std::unordered_map<std::wstring, FileClass>&& m);
bool AnyCarriedClasses();
bool LookupCarriedClass(const std::wstring& srcLower, FileClass& fc);

// The Unreal cache/derived folders the preset skips (proper case, for display
// and for SetExcludedDirs).
const std::vector<std::wstring>& UnrealExcludeNames();

// True if `dir` looks like an Unreal project root: it holds a *.uproject file.
bool IsUnrealProject(const std::wstring& dir);

} // namespace angelcopy
