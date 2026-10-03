#pragma once
#include "Robocopy.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace angelcopy {

struct CompareOutcome {
    bool cancelled = false;   // user aborted the whole transfer
    // "Do the same for all remaining" was ticked: the last pick covers every
    // conflict not decided yet, images AND non-images. Overwrite/Skip arrive
    // as `restPolicy`; Keep both arrives as one Rename decision per file.
    bool restDecided = false;
    Conflict restPolicy = Conflict::Replace;
};

// Per-image conflict prompt: walks the IMAGE conflicts in `items` one at a
// time, source and destination side by side as thumbnails (the shell's own,
// loaded off the UI thread) with size, dimensions and date. Each pick lands
// in `decisions` (lowercased source path -> pick); "Keep both" names come
// from `renamer`. Blocks until done or cancelled.
CompareOutcome AskCompareImages(
    const std::vector<ConflictItem>& items, RenamePlanner& renamer,
    std::unordered_map<std::wstring, FileDecision>& decisions);

} // namespace angelcopy
