// The Search text window's options, remembered between windows in
// %LOCALAPPDATA%\Plugin Shell Extension\search_options.json (like
// folder_views.json), so settings.json stays what it is. A missing or damaged
// file means the defaults.
#pragma once

#include <string>

namespace pse {

struct SearchWindowOptions {
    bool matchCase = false;
    bool wholeWord = false;
    bool regex = false;
    bool otherText = false;
    std::wstring extensions;  // the "Other text files" mask as typed; empty = the default list
    bool decompilePex = false;
};

std::wstring SearchOptionsFilePath();  // PSE_SEARCH_OPTIONS_FILE overrides it (tests)
SearchWindowOptions LoadSearchOptions();
bool SaveSearchOptions(const SearchWindowOptions& options);

}  // namespace pse
