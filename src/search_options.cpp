#include "search_options.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pse {
namespace {

using nlohmann::json;

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

std::wstring SearchOptionsFilePath() {
    wchar_t forced[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"PSE_SEARCH_OPTIONS_FILE", forced, ARRAYSIZE(forced))) return forced;
    PWSTR base = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) folder = std::wstring(base) + L"\\Plugin Shell Extension";
    CoTaskMemFree(base);
    if (!folder.empty()) CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\search_options.json";
}

SearchWindowOptions LoadSearchOptions() {
    SearchWindowOptions o;
    try {
        std::ifstream file(std::filesystem::path(SearchOptionsFilePath()), std::ios::binary);
        if (!file) return o;
        const json root = json::parse(file, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return o;
        auto flag = [&](const char* key, bool& out) {
            auto it = root.find(key);
            if (it != root.end() && it->is_boolean()) out = it->get<bool>();
        };
        flag("matchCase", o.matchCase);
        flag("wholeWord", o.wholeWord);
        flag("regex", o.regex);
        flag("otherText", o.otherText);
        flag("decompilePex", o.decompilePex);
        auto it = root.find("extensions");
        if (it != root.end() && it->is_string()) o.extensions = Widen(it->get<std::string>());
    } catch (...) {
        return SearchWindowOptions{};
    }
    return o;
}

bool SaveSearchOptions(const SearchWindowOptions& o) {
    try {
        const json root = {{"matchCase", o.matchCase}, {"wholeWord", o.wholeWord}, {"regex", o.regex}, {"otherText", o.otherText},
                           {"extensions", Narrow(o.extensions)}, {"decompilePex", o.decompilePex}};
        const std::string text = root.dump(2);
        const std::wstring path = SearchOptionsFilePath();
        const std::wstring temporary = path + L".tmp";
        {
            std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file) return false;
        }
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    } catch (...) {
        return false;
    }
}

}  // namespace pse
