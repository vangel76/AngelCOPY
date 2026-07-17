#pragma once
#include "Delete.h"
#include <string>

namespace angelcopy {

// Mandatory confirmation before a permanent delete. Returns true only if the
// user explicitly confirmed; Cancel is the default so that closing the window,
// Esc, or Enter never deletes anything.
bool AskDeleteConfirm(const DeleteScan& scan);

// Mandatory confirmation before a mirror: shows what will be copied AND what
// will be permanently deleted at the destination (with a list). Same safety
// contract as the delete prompt: Cancel is the default, closing never mirrors.
bool AskSyncConfirm(unsigned long long copyFiles, unsigned long long copyBytes,
                    const DeleteScan& del);

} // namespace angelcopy
