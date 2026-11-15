#include "staging_info.h"

#include <windows.h>

#include <mutex>
#include <vector>

#include "staging_find.h"

namespace pse {
namespace {

constexpr ULONGLONG kRefreshMs = 30000;

struct Root {
    std::wstring dir;  // lower case, one trailing backslash
    bool hardlinks;    // Vortex (or a saved folder): deployed files are hardlinks into Data
};
struct Layout {
    std::wstring data;  // lower case, one trailing backslash; empty = no game found
    std::wstring dataAsIs;
    std::vector<Root> roots;
};

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring WithSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

std::mutex g_lock;
Layout g_layout;
ULONGLONG g_layoutAt = 0;
bool g_layoutSet = false;

Layout Current() {
    std::lock_guard<std::mutex> g(g_lock);
    const ULONGLONG now = GetTickCount64();
    if (g_layoutSet && now - g_layoutAt < kRefreshMs) return g_layout;
    Layout l;
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
        if (!seen) l.roots.push_back({dir, c.manager != "mo2"});
    }
    g_layout = l;
    g_layoutAt = now;
    g_layoutSet = true;
    return l;
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
    const Layout l = Current();
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
    const Layout l = Current();
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
