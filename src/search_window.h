// The "Search text" window: runs a TextSearch (text_search.h) over a folder or
// .bsa and lists the matching lines. It lives on its own thread, so Explorer is
// never blocked, and keeps the DLL loaded until it is closed.
#pragma once

#include <string>

namespace pse {

// `scope` is a folder, a drive or a .bsa file.
void ShowSearchWindowAsync(const std::wstring& scope);

// Runs the window on this thread. With png set (tests), it fills in `query`,
// starts the search, waits for it to finish, saves a picture (and, next to it,
// png + ".txt" with the status line and the rows) and closes.
int RunSearchWindow(const std::wstring& scope, const std::wstring& query, const std::wstring& png);

}  // namespace pse
