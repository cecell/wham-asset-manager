// "Search text..." in the right-click menu of a folder, a drive, the background
// of a folder and a .bsa file.
#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateSearchMenuFactory(REFIID riid, void** ppv);

}  // namespace pse
