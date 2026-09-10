#pragma once
#include <string>
#include <vector>

namespace angelcopy {

// The fast properties dialog (Alt+Enter / "Properties FAST"): opens instantly
// and counts files/folders/bytes live on the parallel scan (8 workers) instead
// of Explorer's serial shell-item walk. "Size on disk" is computed only on
// demand (button). The "Windows properties" button opens the native sheet for
// everything we deliberately do not rebuild (Security, Sharing, Previous
// Versions, ...). Returns when the dialog is closed.
void ShowProps(const std::vector<std::wstring>& targets);

} // namespace angelcopy
