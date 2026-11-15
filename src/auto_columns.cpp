// Auto columns: see auto_columns.h.
//
// How it runs: a background thread in explorer.exe listens to ShellWindows
// (a window opens) and each window's DWebBrowserEvents2 (a folder finished
// loading). A window's view can only be changed on that window's own thread,
// so the watcher posts a registered message to the window and a WH_GETMESSAGE
// hook on that thread (installed once per thread) applies the preset there.
#include "auto_columns.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <exdisp.h>  // after shlobj.h: needs the OLE types it brings in
#include <exdispid.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

#include "debug_log.h"
#include "folder_views.h"

namespace pse {
namespace {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

constexpr size_t kMaxApplied = 5000;
constexpr int kRetries = 15;
constexpr UINT kRetryMs = 200;

std::mutex g_lock;
std::atomic<bool> g_suspended{false};

struct Rule {
    std::wstring folder, preset;
    bool hereOnly = false;
};

struct Store {
    bool enabled = false;
    std::vector<Rule> rules;
    std::map<std::wstring, std::wstring> applied;  // folder (lower case) -> the columns WHAM set
    std::set<std::wstring> manual;
};

std::vector<Rule> DefaultRules() {
    return {{L"textures", L"Textures"}, {L"meshes", L"Meshes"},        {L"animations", L"Animations"}, {L"behaviors", L"Animations"},
            {L"behaviours", L"Animations"}, {L"sound", L"Audio"},       {L"music", L"Audio"},           {L"scripts", L"Scripts"},
            {L"data", L"Plugins", true}};
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring FolderKey(const std::wstring& folder) {
    std::wstring k = Lower(folder);
    while (k.size() > 3 && (k.back() == L'\\' || k.back() == L'/')) k.pop_back();
    return k;
}

std::wstring StorePath() {
    wchar_t forced[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"PSE_AUTO_COLUMNS_FILE", forced, ARRAYSIZE(forced))) return forced;
    std::wstring views = FolderViewsFilePath();
    return views.substr(0, views.find_last_of(L'\\') + 1) + L"auto_columns.json";
}

// The store, read again when the file changes (another Explorer process, or the user editing rules).
Store g_store;
FILETIME g_storeTime{};
bool g_storeLoaded = false;

FILETIME WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}

Store& Loaded() {  // g_lock held
    const std::wstring path = StorePath();
    const FILETIME t = WriteTime(path);
    if (g_storeLoaded && CompareFileTime(&t, &g_storeTime) == 0) return g_store;
    Store s;
    s.rules = DefaultRules();
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (f) {
        const json j = json::parse(f, nullptr, false);
        if (j.is_object()) {
            s.enabled = j.value("enabled", false);
            if (j.contains("rules") && j["rules"].is_array()) {
                s.rules.clear();
                for (const json& r : j["rules"])
                    if (r.is_object() && r.contains("folder") && r.contains("preset"))
                        s.rules.push_back({Lower(Wide(r["folder"].get<std::string>())), Wide(r["preset"].get<std::string>()), r.value("here_only", false)});
            }
            if (j.contains("applied") && j["applied"].is_object())
                for (auto it = j["applied"].begin(); it != j["applied"].end(); ++it)
                    if (it.value().is_string()) s.applied[Wide(it.key())] = Wide(it.value().get<std::string>());
            if (j.contains("manual") && j["manual"].is_array())
                for (const json& m : j["manual"])
                    if (m.is_string()) s.manual.insert(Wide(m.get<std::string>()));
        }
    }
    g_store = std::move(s);
    g_storeTime = t;
    g_storeLoaded = true;
    return g_store;
}

void Save(const Store& s) {  // g_lock held
    json rules = json::array(), applied = json::object(), manual = json::array();
    for (const Rule& r : s.rules) {
        json o = {{"folder", Utf8(r.folder)}, {"preset", Utf8(r.preset)}};
        if (r.hereOnly) o["here_only"] = true;
        rules.push_back(o);
    }
    for (const auto& [k, v] : s.applied) applied[Utf8(k)] = Utf8(v);
    for (const auto& m : s.manual) manual.push_back(Utf8(m));
    const std::string text = json{{"enabled", s.enabled}, {"rules", rules}, {"manual", manual}, {"applied", applied}}.dump(1);
    const std::wstring path = StorePath(), temp = path + L".tmp";
    {
        std::ofstream f(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_storeTime = WriteTime(path);
}

bool HasPlugins(const std::wstring& folder) {
    for (const wchar_t* pat : {L"\\*.esp", L"\\*.esm", L"\\*.esl"}) {
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileExW((folder + pat).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            return true;
        }
    }
    return false;
}

std::wstring PresetFor(const Store& s, const std::wstring& folder) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : folder + L"\\") {
        if (c == L'\\' || c == L'/') {
            if (!cur.empty()) parts.push_back(Lower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (size_t i = parts.size(); i-- > 0;)
        for (const Rule& r : s.rules) {
            if (parts[i] != r.folder || (r.hereOnly && i + 1 != parts.size())) continue;
            if (r.preset == L"Plugins" && !HasPlugins(folder)) continue;  // a Data folder of something else
            return r.preset;
        }
    return HasPlugins(folder) ? L"Plugins" : L"";  // a mod's own folder
}

std::wstring FolderOf(IShellView* view) {
    ComPtr<IFolderView> fv;
    ComPtr<IPersistFolder2> pf;
    if (!view || FAILED(view->QueryInterface(IID_PPV_ARGS(&fv))) || FAILED(fv->GetFolder(IID_PPV_ARGS(&pf)))) return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(pf->GetCurFolder(&pidl))) return {};
    wchar_t path[MAX_PATH * 2] = {};
    const BOOL ok = SHGetPathFromIDListEx(pidl, path, ARRAYSIZE(path), GPFIDL_DEFAULT);
    CoTaskMemFree(pidl);
    return ok ? path : L"";
}

std::wstring Signature(IShellView* view) {
    ComPtr<IColumnManager> cm;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(&cm)))) return {};
    UINT n = 0;
    if (FAILED(cm->GetColumnCount(CM_ENUM_VISIBLE, &n)) || !n) return {};
    std::vector<PROPERTYKEY> keys(n);
    if (FAILED(cm->GetColumns(CM_ENUM_VISIBLE, keys.data(), n))) return {};
    std::wstring s;
    for (const PROPERTYKEY& k : keys) {
        wchar_t text[PKEYSTR_MAX] = {};
        PSStringFromPropertyKey(k, text, ARRAYSIZE(text));
        s += std::wstring(text) + L"|";
    }
    return s;
}

// ---- The watcher ----

UINT g_applyMessage = 0;
std::map<DWORD, HHOOK> g_hooks;  // per Explorer UI thread (g_lock)

// The window's browser, looked up on the window's own thread.
ComPtr<IShellBrowser> BrowserOf(HWND hwnd) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return nullptr;
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; ++i) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        ComPtr<IDispatch> disp;
        if (FAILED(windows->Item(v, &disp)) || !disp) continue;
        ComPtr<IWebBrowser2> wb;
        SHANDLE_PTR h = 0;
        if (FAILED(disp.As(&wb)) || FAILED(wb->get_HWND(&h)) || reinterpret_cast<HWND>(h) != hwnd) continue;
        ComPtr<IShellBrowser> browser;
        if (SUCCEEDED(IUnknown_QueryService(wb.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return browser;
    }
    return nullptr;
}

struct Retry {
    HWND hwnd;
    int attempt;
};
thread_local std::map<UINT_PTR, Retry> t_retries;

void ApplyOnThread(HWND hwnd, int attempt);

void CALLBACK RetryTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto it = t_retries.find(id);
    if (it == t_retries.end()) return;
    const Retry r = it->second;
    t_retries.erase(it);
    ApplyOnThread(r.hwnd, r.attempt);
}

void ApplyOnThread(HWND hwnd, int attempt) {
    if (!IsWindow(hwnd)) return;
    ComPtr<IShellBrowser> browser = BrowserOf(hwnd);
    const std::wstring note = browser ? ApplyAutoColumnsNow(browser.Get()) : L"no browser";
    // The view may still be loading: try again shortly.
    if ((note == L"no browser" || note == L"no view") && attempt < kRetries) {
        const UINT_PTR id = SetTimer(nullptr, 0, kRetryMs, &RetryTimer);
        if (id) t_retries[id] = {hwnd, attempt + 1};
    }
}

LRESULT CALLBACK GetMessageHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        auto* msg = reinterpret_cast<MSG*>(lp);
        if (g_applyMessage && msg->message == g_applyMessage) {
            const HWND hwnd = msg->hwnd;
            msg->message = WM_NULL;  // ours: the window never sees it
            ApplyOnThread(hwnd, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Request(HWND hwnd) {
    if (!hwnd || g_suspended) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!Loaded().enabled) return;
    }
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return;  // another Explorer process watches its own windows
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!g_hooks.count(tid)) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&GetMessageHook), &self);
            HHOOK h = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, self, tid);
            if (!h) return;
            g_hooks[tid] = h;
        }
    }
    PostMessageW(hwnd, g_applyMessage, 0, 0);
}

constexpr UINT kMsgDetach = WM_APP + 1;  // to the watcher thread: lParam = a closing window

class Sink : public IDispatch {
public:
    Sink(REFIID iid, HWND hwnd, std::function<void()> onWindows) : iid_(iid), hwnd_(hwnd), onWindows_(std::move(onWindows)) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == iid_) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return static_cast<ULONG>(r);
    }
    IFACEMETHODIMP GetTypeInfoCount(UINT* n) override {
        *n = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (iid_ == DIID_DWebBrowserEvents2 && id == DISPID_DOCUMENTCOMPLETE) Request(hwnd_);
        // The window is closing: let go of it at once, or Explorer can't finish closing it.
        if (iid_ == DIID_DWebBrowserEvents2 && id == DISPID_ONQUIT) PostThreadMessageW(GetCurrentThreadId(), kMsgDetach, 0, reinterpret_cast<LPARAM>(hwnd_));
        if (iid_ == DIID_DShellWindowsEvents && (id == DISPID_WINDOWREGISTERED || id == DISPID_WINDOWREVOKED) && onWindows_) onWindows_();
        return S_OK;
    }

private:
    virtual ~Sink() = default;
    long refs_ = 1;
    IID iid_;
    HWND hwnd_;
    std::function<void()> onWindows_;
};

struct Attached {
    ComPtr<IConnectionPoint> cp;
    DWORD cookie = 0;
};

DWORD WINAPI WatchThread(void*) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 0;
    {
        ComPtr<IShellWindows> windows;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) {
            std::map<HWND, Attached> attached;
            auto detach = [&](HWND hwnd) {
                auto it = attached.find(hwnd);
                if (it == attached.end()) return;
                it->second.cp->Unadvise(it->second.cookie);
                attached.erase(it);
            };
            auto attachAll = [&] {
                // Windows ShellWindows no longer lists are closing: let go of them.
                std::set<HWND> listed;
                long count = 0;
                windows->get_Count(&count);
                for (long i = 0; i < count; ++i) {
                    VARIANT v;
                    VariantInit(&v);
                    v.vt = VT_I4;
                    v.lVal = i;
                    ComPtr<IDispatch> disp;
                    ComPtr<IWebBrowser2> wb;
                    SHANDLE_PTR h = 0;
                    if (SUCCEEDED(windows->Item(v, &disp)) && disp && SUCCEEDED(disp.As(&wb)) && SUCCEEDED(wb->get_HWND(&h)))
                        listed.insert(reinterpret_cast<HWND>(h));
                }
                for (auto it = attached.begin(); it != attached.end();) {
                    if (!listed.count(it->first) || !IsWindow(it->first)) {
                        it->second.cp->Unadvise(it->second.cookie);
                        it = attached.erase(it);
                    } else {
                        ++it;
                    }
                }
                count = 0;
                windows->get_Count(&count);
                for (long i = 0; i < count; ++i) {
                    VARIANT v;
                    VariantInit(&v);
                    v.vt = VT_I4;
                    v.lVal = i;
                    ComPtr<IDispatch> disp;
                    ComPtr<IWebBrowser2> wb;
                    SHANDLE_PTR h = 0;
                    if (FAILED(windows->Item(v, &disp)) || !disp || FAILED(disp.As(&wb)) || FAILED(wb->get_HWND(&h))) continue;
                    const HWND hwnd = reinterpret_cast<HWND>(h);
                    if (attached.count(hwnd)) continue;
                    ComPtr<IConnectionPointContainer> cpc;
                    ComPtr<IConnectionPoint> cp;
                    if (FAILED(wb.As(&cpc)) || FAILED(cpc->FindConnectionPoint(DIID_DWebBrowserEvents2, &cp))) continue;
                    auto* sink = new Sink(DIID_DWebBrowserEvents2, hwnd, nullptr);
                    DWORD cookie = 0;
                    if (SUCCEEDED(cp->Advise(sink, &cookie))) {
                        attached[hwnd] = {cp, cookie};
                        Request(hwnd);  // the folder it shows now
                    }
                    sink->Release();
                }
            };
            ComPtr<IConnectionPointContainer> cpc;
            ComPtr<IConnectionPoint> cp;
            DWORD cookie = 0;
            auto* sink = new Sink(DIID_DShellWindowsEvents, nullptr, attachAll);
            if (SUCCEEDED(windows.As(&cpc)) && SUCCEEDED(cpc->FindConnectionPoint(DIID_DShellWindowsEvents, &cp))) cp->Advise(sink, &cookie);
            sink->Release();
            attachAll();
            // When the shell exits (the taskbar goes away: "Exit Explorer", a
            // restart, sign-out) the watcher lets go of everything and ends, so
            // Explorer can close its windows and save them for next time.
            const UINT_PTR timer = SetTimer(nullptr, 0, 500, nullptr);
            bool sawTray = FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (!msg.hwnd && msg.message == kMsgDetach) {
                    detach(reinterpret_cast<HWND>(msg.lParam));
                    continue;
                }
                if (!msg.hwnd && msg.message == WM_TIMER && msg.wParam == timer) {
                    const bool tray = FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
                    if (sawTray && !tray) break;
                    sawTray = sawTray || tray;
                    continue;
                }
                DispatchMessageW(&msg);
            }
            KillTimer(nullptr, timer);
            if (cp && cookie) cp->Unadvise(cookie);
            for (auto& [h, a] : attached) a.cp->Unadvise(a.cookie);
            attached.clear();
            std::lock_guard<std::mutex> g(g_lock);
            for (auto& [tid, h] : g_hooks) UnhookWindowsHookEx(h);
            g_hooks.clear();
        }
    }
    CoUninitialize();
    return 0;
}

bool IsExplorerProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return _wcsicmp(PathFindFileNameW(path), L"explorer.exe") == 0;
}

}  // namespace

bool AutoColumnsEnabled() {
    std::lock_guard<std::mutex> g(g_lock);
    return Loaded().enabled;
}

void SetAutoColumnsEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> g(g_lock);
        Store& s = Loaded();
        s.enabled = enabled;
        Save(s);
    }
    if (enabled) StartAutoColumns();
}

std::wstring AutoPresetFor(const std::wstring& folder) {
    std::lock_guard<std::mutex> g(g_lock);
    return PresetFor(Loaded(), folder);
}

void MarkFolderManual(const std::wstring& folder) {
    std::lock_guard<std::mutex> g(g_lock);
    Store& s = Loaded();
    const std::wstring k = FolderKey(folder);
    s.manual.insert(k);
    s.applied.erase(k);
    Save(s);
}

void MarkFoldersManual(const std::vector<std::wstring>& folders) {
    if (folders.empty()) return;
    std::lock_guard<std::mutex> g(g_lock);
    Store& s = Loaded();
    for (const std::wstring& f : folders) {
        const std::wstring k = FolderKey(f);
        s.manual.insert(k);
        s.applied.erase(k);
    }
    Save(s);
}

bool IsFolderManual(const std::wstring& folder) {
    std::lock_guard<std::mutex> g(g_lock);
    return Loaded().manual.count(FolderKey(folder)) != 0;
}

void ResetFolderToAuto(const std::wstring& folder) {
    std::lock_guard<std::mutex> g(g_lock);
    Store& s = Loaded();
    const std::wstring k = FolderKey(folder);
    s.manual.erase(k);
    s.applied.erase(k);
    Save(s);
}

void SuspendAutoColumns(bool suspend) { g_suspended = suspend; }

std::wstring ApplyAutoColumnsNow(IShellBrowser* browser) {
    if (g_suspended) return L"suspended";
    ComPtr<IShellView> view;
    if (!browser || FAILED(browser->QueryActiveShellView(&view)) || !view) return L"no view";
    const std::wstring folder = FolderOf(view.Get());
    if (folder.empty()) return L"not a folder";
    std::wstring preset;
    const std::wstring key = FolderKey(folder);
    std::wstring last;
    {
        std::lock_guard<std::mutex> g(g_lock);
        Store& s = Loaded();
        if (!s.enabled) return L"off";
        preset = PresetFor(s, folder);
        if (preset.empty()) return L"no rule";
        if (s.manual.count(key)) return L"manual";
        auto it = s.applied.find(key);
        if (it != s.applied.end()) last = it->second;
    }
    const std::wstring now = Signature(view.Get());
    if (!last.empty()) {
        if (last == now) return L"already " + preset;
        MarkFolderManual(folder);  // the user changed what WHAM set: theirs from now on
        return L"changed by hand";
    }
    const std::vector<FolderViewState> views = LoadFolderViews();
    auto state = std::find_if(views.begin(), views.end(), [&](const FolderViewState& v) { return _wcsicmp(v.name.c_str(), preset.c_str()) == 0; });
    if (state == views.end()) return L"no preset " + preset;
    if (FAILED(ApplyFolderView(view.Get(), *state))) return L"failed";
    const std::wstring after = Signature(view.Get());
    std::lock_guard<std::mutex> g(g_lock);
    Store& s = Loaded();
    if (s.applied.size() >= kMaxApplied) s.applied.erase(s.applied.begin());
    s.applied[key] = after;
    Save(s);
    return L"applied " + preset;
}

void StartAutoColumns() {
    static std::once_flag once;
    if (!IsExplorerProcess()) return;
    std::call_once(once, [] {
        g_applyMessage = RegisterWindowMessageW(L"Cecell.PluginShellExtension.AutoColumns");
        // The watcher and the hooks live as long as Explorer: keep the DLL loaded.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&StartAutoColumns), &self);
        if (HANDLE t = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr)) CloseHandle(t);
    });
}

}  // namespace pse
