#include "Common.h"

// {7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D02}
const CLSID CLSID_AngelContextMenu = {
    0x7f3a9c21, 0x1b4e, 0x4c8a,
    {0x9e, 0x2d, 0x4a, 0x1f, 0x6b, 0x0c, 0x0d, 0x02}};

// {7F3A9C21-1B4E-4C8A-9E2D-4A1F6B0C0D03} — right-drag handler.
const CLSID CLSID_AngelDragDrop = {
    0x7f3a9c21, 0x1b4e, 0x4c8a,
    {0x9e, 0x2d, 0x4a, 0x1f, 0x6b, 0x0c, 0x0d, 0x03}};

// ...0D01 was the old DropHandler class. It is gone: the shell never asked for
// it. Only its string form survives, in Common.h, for uninstalling the leftovers.
