#pragma once
#include "Robocopy.h"
#include <string>
#include <vector>

namespace angelcopy {

// Explorer-style conflict prompt, shown before a transfer when the destination
// already holds files that differ from the source. Blocks until the user picks.
// `cancelled` is set when the user aborts the whole transfer.
// `offerEach` adds "Compare images one by one…"; picking it sets *decideEach
// (the returned policy is then meaningless — the compare dialog decides).
Conflict AskConflict(Operation op, unsigned long long conflictCount,
                     const std::vector<std::wstring>& sample, bool& cancelled,
                     bool offerEach = false, bool* decideEach = nullptr);

} // namespace angelcopy
