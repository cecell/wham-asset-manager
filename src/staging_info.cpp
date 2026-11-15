#include "staging_info.h"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "staging_find.h"

namespace pse {
namespace {

constexpr ULONGLONG kRefreshMs = 30000;

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring WithSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// An MO2 profile's mod order. MO2 deploys nothing: while the game runs it shows
// the mods as if they were in Data, the first mod of modlist.txt winning.
struct Mo2Profile {
    std::wstring mods;       // the mods folder, as is, with a trailing backslash
    std::wstring overwrite;  // the overwrite folder (wins over every mod)
    std::vector<std::wstring> names, lowerNames;  // modlist.txt order: first = highest priority
    std::vector<bool> enabled;
    std::mutex lock;
    std::map<std::wstring, std::vector<size_t>> withDir;  // a relative folder -> the enabled mods that have it

    size_t Find(const std::wstring& lowerName) const {
        for (size_t i = 0; i < lowerNames.size(); ++i)
            if (lowerNames[i] == lowerName) return i;
        return SIZE_MAX;
    }
    // The enabled mods that have `dir` (lower case, "" or "textures\\armor\\"), in order.
    std::vector<size_t> ModsWithDir(const std::wstring& dir) {
        std::lock_guard<std::mutex> g(lock);
        auto it = withDir.find(dir);
        if (it != withDir.end()) return it->second;
        std::vector<size_t> out;
        for (size_t i = 0; i < names.size(); ++i)
            if (enabled[i] && (dir.empty() || Exists(mods + names[i] + L"\\" + dir))) out.push_back(i);
        if (withDir.size() > 2000) withDir.clear();
        withDir[dir] = out;
        return out;
    }
};

std::shared_ptr<Mo2Profile> ReadMo2Profile(const StagingCandidate& c) {
    if (c.instance.empty() || c.profile.empty()) return nullptr;
    const Mo2Folders f = ResolveMo2Folders(c.instance);
    const int len = MultiByteToWideChar(CP_UTF8, 0, c.profile.data(), static_cast<int>(c.profile.size()), nullptr, 0);
    std::wstring profile(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, c.profile.data(), static_cast<int>(c.profile.size()), profile.data(), len);
    std::ifstream in(std::filesystem::path(WithSlash(f.profiles) + profile + L"\\modlist.txt"), std::ios::binary);
    if (!in) return nullptr;
    auto p = std::make_shared<Mo2Profile>();
    p->mods = WithSlash(f.mods);
    p->overwrite = WithSlash(f.overwrite);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 2 || line[0] == '#' || line[0] == '*') continue;  // * = DLC and other files MO2 doesn't manage
        const bool on = line[0] != '-';
        if (line[0] == '+' || line[0] == '-') line.erase(0, 1);
        const int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring name(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), name.data(), n);
        p->names.push_back(name);
        p->lowerNames.push_back(Lower(name));
        p->enabled.push_back(on);
    }
    return p;
}

struct Root {
    std::wstring dir;  // lower case, one trailing backslash
    bool hardlinks;    // Vortex (or a saved folder): deployed files are hardlinks into Data
    std::shared_ptr<Mo2Profile> mo2;  // MO2: the selected profile's order
};
struct Layout {
    std::wstring data;  // lower case, one trailing backslash; empty = no game found
    std::wstring dataAsIs;
    std::vector<Root> roots;
};

std::mutex g_lock;
std::shared_ptr<const Layout> g_layout;
ULONGLONG g_layoutAt = 0;

std::shared_ptr<const Layout> Current() {
    std::lock_guard<std::mutex> g(g_lock);
    const ULONGLONG now = GetTickCount64();
    if (g_layout && now - g_layoutAt < kRefreshMs) return g_layout;
    auto made = std::make_shared<Layout>();
    Layout& l = *made;
    StagingResult r;
    std::string error;
    const StagingEnv env = CurrentStagingEnv();
    FindStaging(L"", "", env, &r, &error);  // even when it can't choose, r.all lists what it found
    if (!r.data.empty()) {
        l.dataAsIs = WithSlash(r.data);
        l.data = Lower(l.dataAsIs);
    }
    for (const StagingCandidate& c : r.all) {
        if (c.staging.empty()) continue;
        const std::wstring dir = Lower(WithSlash(c.staging));
        bool seen = false;
        for (const Root& x : l.roots) seen = seen || x.dir == dir;
        if (seen) continue;
        if (c.manager == "mo2") l.roots.push_back({dir, false, ReadMo2Profile(c)});
        else l.roots.push_back({dir, true, nullptr});
    }
    g_layout = made;
    g_layoutAt = now;
    return g_layout;
}

// Deployed for a file in an MO2 mod (`rest` = "Mod\textures\a.dds" as is).
std::wstring Mo2Deployed(Mo2Profile& p, const std::wstring& rest) {
    const size_t slash = rest.find(L'\\');
    if (slash == std::wstring::npos) return {};
    const std::wstring rel = rest.substr(slash + 1);
    const size_t self = p.Find(Lower(rest.substr(0, slash)));
    if (self == SIZE_MAX) return L"No (not in the MO2 profile)";
    if (!p.enabled[self]) return L"No (disabled in MO2)";
    if (Exists(p.overwrite + rel)) return L"No (Overwrite wins)";
    const size_t cut = rel.find_last_of(L'\\');
    const std::wstring dir = cut == std::wstring::npos ? std::wstring() : Lower(rel.substr(0, cut + 1));
    for (size_t i : p.ModsWithDir(dir)) {
        if (i >= self) break;
        if (Exists(p.mods + p.names[i] + L"\\" + rel)) return L"No (" + p.names[i] + L" wins)";
    }
    return L"Yes";
}

// The root the path is in, and the part after it ("Mod\textures\a.dds").
const Root* RootOf(const Layout& l, const std::wstring& lowerPath, std::wstring* rest) {
    for (const Root& r : l.roots)
        if (lowerPath.size() > r.dir.size() && lowerPath.compare(0, r.dir.size(), r.dir) == 0) {
            *rest = lowerPath.substr(r.dir.size());
            return &r;
        }
    return nullptr;
}

std::wstring ModIn(const Layout& l, const std::wstring& path) {
    std::wstring rest;
    if (!RootOf(l, Lower(path), &rest)) return {};
    const size_t slash = rest.find(L'\\');
    if (slash == std::wstring::npos) return {};  // a file in the staging folder itself
    // the mod folder's own spelling, from the original path
    return path.substr(path.size() - rest.size(), slash);
}

bool SameFile(const std::wstring& a, const std::wstring& b) {
    auto id = [](const std::wstring& p, BY_HANDLE_FILE_INFORMATION* info) {
        HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const BOOL ok = GetFileInformationByHandle(h, info);
        CloseHandle(h);
        return ok != FALSE;
    };
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    return id(a, &x) && id(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
           x.nFileIndexLow == y.nFileIndexLow;
}

// Every other name of the same file (hardlinks), as full paths.
std::vector<std::wstring> OtherNames(const std::wstring& path) {
    std::vector<std::wstring> out;
    if (path.size() < 3 || path[1] != L':') return out;
    const std::wstring drive = path.substr(0, 2), self = Lower(path);
    std::wstring name(MAX_PATH * 2, L'\0');
    DWORD len = static_cast<DWORD>(name.size());
    HANDLE find = FindFirstFileNameW(path.c_str(), 0, &len, name.data());
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring other = drive + std::wstring(name.c_str());
        if (Lower(other) != self) out.push_back(other);
        len = static_cast<DWORD>(name.size());
    } while (FindNextFileNameW(find, &len, name.data()));
    FindClose(find);
    return out;
}

// A game's Data folder: a folder named Data next to SkyrimSE.exe. Returns it (with
// a trailing backslash) when `path` is inside one.
std::wstring GameDataOf(const std::wstring& path) {
    const std::wstring lower = Lower(path);
    for (size_t at = lower.find(L"\\data\\"); at != std::wstring::npos; at = lower.find(L"\\data\\", at + 1)) {
        const std::wstring game = path.substr(0, at + 1);
        if (GetFileAttributesW((game + L"SkyrimSE.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return path.substr(0, at + 6);
    }
    return {};
}

// The mod a Data file was deployed from: another name of the same file, in a
// known staging folder, or anywhere outside a game folder that ends in the
// same relative path ("D:\\MyStaging\\Some Mod\\Data\\textures\\a.dds" -> "Some Mod").
std::wstring ModOfDataFile(const Layout& l, const std::wstring& path) {
    const std::wstring data = GameDataOf(path);
    if (data.empty()) return {};
    const std::wstring rel = Lower(path.substr(data.size()));
    std::wstring guess;
    for (const std::wstring& other : OtherNames(path)) {
        if (std::wstring mod = ModIn(l, other); !mod.empty()) return mod;
        if (!guess.empty() || !GameDataOf(other).empty()) continue;  // another game's Data is not a mod
        const std::wstring lo = Lower(other);
        if (lo.size() <= rel.size() + 1 || lo.compare(lo.size() - rel.size(), rel.size(), rel) != 0 || lo[lo.size() - rel.size() - 1] != L'\\') continue;
        std::wstring root = other.substr(0, other.size() - rel.size() - 1);
        if (root.size() > 5 && Lower(root.substr(root.size() - 5)) == L"\\data") root.resize(root.size() - 5);
        const size_t slash = root.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < root.size()) guess = root.substr(slash + 1);
    }
    return guess;
}

}  // namespace

std::wstring ModOf(const std::wstring& path) {
    const std::shared_ptr<const Layout> current = Current();
    const Layout& l = *current;
    if (std::wstring mod = ModIn(l, path); !mod.empty()) return mod;
    if (!GameDataOf(path).empty()) return ModOfDataFile(l, path);  // in any game's Data (the active one, or a parked mode)
    // A staging folder no manager names (a deploy script's): the link in a game's
    // Data tells the relative path, and what is left before it is the mod's folder.
    for (const std::wstring& other : OtherNames(path)) {
        const std::wstring data = GameDataOf(other);
        if (data.empty()) continue;
        const std::wstring rel = Lower(other.substr(data.size())), lo = Lower(path);
        if (lo.size() <= rel.size() + 1 || lo.compare(lo.size() - rel.size(), rel.size(), rel) != 0) continue;
        std::wstring root = path.substr(0, path.size() - rel.size() - 1);
        if (root.size() > 5 && Lower(root.substr(root.size() - 5)) == L"\\data") root.resize(root.size() - 5);
        const size_t slash = root.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < root.size()) return root.substr(slash + 1);
    }
    return {};
}

std::wstring DeployedOf(const std::wstring& path) {
    const std::shared_ptr<const Layout> current = Current();
    const Layout& l = *current;
    if (l.data.empty()) return {};
    if (!GameDataOf(path).empty()) return {};  // a game's own files
    // Whoever deployed it: a hardlink of this very file in the game's Data
    std::wstring elsewhere;
    for (const std::wstring& other : OtherNames(path)) {
        if (Lower(other).compare(0, l.data.size(), l.data) == 0) return L"Yes";
        const std::wstring d = GameDataOf(other);
        if (!d.empty() && elsewhere.empty()) {
            std::wstring game = d.substr(0, d.size() - 6);  // the game folder
            elsewhere = L"Yes (in " + game.substr(game.find_last_of(L'\\') + 1) + L")";
        }
    }
    std::wstring rest;
    const Root* root = RootOf(l, Lower(path), &rest);
    if (root && root->mo2) return Mo2Deployed(*root->mo2, path.substr(path.size() - rest.size()));
    if (!root || !root->hardlinks) return elsewhere;
    const size_t slash = rest.find(L'\\');
    if (slash == std::wstring::npos) return {};
    const std::wstring rel = path.substr(path.size() - rest.size() + slash + 1);
    const std::wstring inData = l.dataAsIs + rel;
    if (GetFileAttributesW(inData.c_str()) == INVALID_FILE_ATTRIBUTES) return elsewhere.empty() ? L"No" : elsewhere;
    if (SameFile(path, inData)) return L"Yes";
    const std::wstring winner = ModOfDataFile(l, inData);
    return winner.empty() ? L"No (a loose file wins)" : L"No (" + winner + L" wins)";
}

}  // namespace pse
