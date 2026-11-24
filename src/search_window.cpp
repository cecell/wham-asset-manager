#include "search_window.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include "property_window.h"
#include "search_options.h"
#include "text_search.h"
#include "window_common.h"

extern HINSTANCE g_hInst;

namespace pse {
namespace {

constexpr wchar_t kSearchClass[] = L"PluginShellExtensionSearch";
constexpr UINT WM_APP_OPENED = WM_APP + 10;  // an open worker finished (error text in the shared state)
constexpr UINT WM_APP_SNAP = WM_APP + 11;    // tests: the search is done, save the picture
constexpr UINT WM_APP_FIT = WM_APP + 12;     // a column edge was dragged: refit the Text column
constexpr UINT_PTR kTimerCopied = 1, kTimerTick = 2;
constexpr UINT kTickMs = 100;

enum {
    IDC_FIND = 301, IDC_IN, IDC_MATCHCASE, IDC_WHOLEWORD, IDC_REGEX, IDC_OTHER, IDC_MASK, IDC_DECOMP,
    IDC_CANCELSEARCH, IDC_RESULTS, IDC_COPYRES, IDC_CLOSEBTN, IDC_LABEL_BASE = 330,
    IDC_STATUS = 360, IDC_LBL_FIND, IDC_LBL_IN,
    IDM_OPEN = 401, IDM_COPYPATH, IDM_COPYLINE
};

constexpr int kWidth = 900, kHeight = 600, kPad = 14, kRowH = 26, kButtonW = 100, kButtonH = 26, kLabelW = 44;

// ---- helpers ----

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring WindowText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    const int got = GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(got > 0 ? got : 0));
    return s;
}

// With the user's thousands separator.
std::wstring Num(uint64_t v) {
    wchar_t plain[32];
    swprintf_s(plain, L"%llu", static_cast<unsigned long long>(v));
    wchar_t dec[8] = L".", thou[8] = L",";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, ARRAYSIZE(dec));
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thou, ARRAYSIZE(thou));
    NUMBERFMTW fmt{0, 1, 3, dec, thou, 1};
    wchar_t out[64];
    if (GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, plain, &fmt, out, ARRAYSIZE(out)) > 0) return out;
    return plain;
}

std::wstring Count(uint64_t n, const wchar_t* one, const wchar_t* many) { return Num(n) + L" " + (n == 1 ? one : many); }

std::wstring OneDecimal(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", v);
    return buf;
}

// Bytes from an archive's path: UTF-8 if valid, else Windows-1252.
std::wstring FromBytes(const std::string& s) {
    if (s.empty()) return {};
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        cp = 1252;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    }
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;

// Tabs shown as four spaces; `ranges` are moved to match.
void ExpandTabs(const std::wstring& in, const Ranges& ranges, std::wstring& out, Ranges& outRanges) {
    out.clear();
    std::vector<uint32_t> map(in.size() + 1);
    for (size_t i = 0; i < in.size(); ++i) {
        map[i] = static_cast<uint32_t>(out.size());
        if (in[i] == L'\t') out.append(4, L' ');
        else out.push_back(in[i]);
    }
    map[in.size()] = static_cast<uint32_t>(out.size());
    outRanges.clear();
    for (const auto& r : ranges) {
        const size_t a = (std::min)(static_cast<size_t>(r.first), in.size()), b = (std::min)(static_cast<size_t>(r.second), in.size());
        if (b > a) outRanges.push_back({map[a], map[b]});
    }
}

std::wstring ScopeTitleName(const std::wstring& scope) {
    std::wstring s = scope;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    const wchar_t* leaf = PathFindFileNameW(s.c_str());
    return (leaf && *leaf && s.size() > 3) ? std::wstring(leaf) : s;
}

// What an open worker tells the window.
struct OpenState {
    std::mutex lock;
    std::wstring error;
};

struct OpenArgs {
    TextSearchFile file;
    uint32_t line;
    HWND hwnd;
    std::shared_ptr<OpenState> state;
    HMODULE module;
};

DWORD WINAPI OpenThread(void* param) {
    std::unique_ptr<OpenArgs> a(static_cast<OpenArgs*>(param));
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    try {
        std::wstring path, error;
        if (WriteSearchCopy(a->file, path, error)) OpenInEditor(path, a->line, error);
        if (!error.empty()) {
            std::lock_guard<std::mutex> g(a->state->lock);
            a->state->error = error;
        }
        PostMessageW(a->hwnd, WM_APP_OPENED, 0, 0);
    } catch (...) {
    }
    if (SUCCEEDED(co)) CoUninitialize();
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

DWORD WINAPI CleanUpThread(void* param) {
    try {
        CleanUpSearchCopies();
    } catch (...) {
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(param), 0);
}

// Runs `routine` on a new thread. `module` is the reference AddModuleRef took, which the
// thread gives back with FreeLibraryAndExitThread; false (reference released) if it
// couldn't start.
bool StartThread(LPTHREAD_START_ROUTINE routine, void* arg, HMODULE module) {
    if (!module) return false;
    if (HANDLE t = CreateThread(nullptr, 0, routine, arg, 0, nullptr)) {
        CloseHandle(t);
        return true;
    }
    FreeLibrary(module);
    return false;
}

// ---- the window ----

class SearchWindow {
public:
    SearchWindow(std::wstring scope, std::wstring query, std::wstring png) : scope_(std::move(scope)), query_(std::move(query)), png_(std::move(png)) {}

    static void Register() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &SearchWindow::StaticProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kSearchClass;
        RegisterClassExW(&wc);
    }

    HWND Create() {
        const std::wstring title = L"Search text - " + ScopeTitleName(scope_);
        return CreateWindowExW(WS_EX_CONTROLPARENT, kSearchClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, g_hInst, this);
    }

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<SearchWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        try {
            return self->Proc(msg, wp, lp);
        } catch (...) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    // The column header draws its text black even in dark mode: colour it here;
    // and a dragged header edge refits the Text column.
    static LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        if (msg == WM_NOTIFY) {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(h)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    SetTextColor(cd->hdc, reinterpret_cast<SearchWindow*>(ref)->theme_.text);
                    return CDRF_DODEFAULT;
                }
            }
            if (nm->code == HDN_ENDTRACKW && nm->hwndFrom == ListView_GetHeader(h)) {
                const LRESULT r = DefSubclassProc(h, msg, wp, lp);
                PostMessageW(GetParent(h), WM_APP_FIT, 0, 0);
                return r;
            }
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, &SearchWindow::ListProc, 1);
        return DefSubclassProc(h, msg, wp, lp);
    }

    int S(int v) const { return Scale(v, dpi_); }

    // ---- layout ----

    int TextWidth(const std::wstring& text) const {
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, font_);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        return sz.cx;
    }

    void PlaceBox(const RECT& box, HWND edit, int slot) {
        boxes_[slot] = box;
        MoveWindow(edit, box.left + S(6), box.top + S(5), box.right - box.left - S(12), S(16), TRUE);
    }

    void PlaceCheck(int slot, int x, int y) {
        MoveWindow(checks_[slot], x, y + S(4), S(18), S(18), TRUE);
        const int w = TextWidth(WindowText(labels_[slot])) + S(6);
        MoveWindow(labels_[slot], x + S(22), y + S(3), w, S(20), TRUE);
        checkRight_[slot] = x + S(22) + w;
    }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int pad = S(kPad), editX = pad + S(kLabelW), rowH = S(kRowH), btnW = S(kButtonW), btnH = S(kButtonH);
        int y = S(12);
        MoveWindow(lblFind_, pad, y + S(4), S(kLabelW) - S(4), S(18), TRUE);
        const int cancelX = rc.right - pad - btnW, searchX = cancelX - S(8) - btnW;
        PlaceBox({editX, y, searchX - S(10), y + rowH}, find_, 0);
        MoveWindow(searchBtn_, searchX, y, btnW, btnH, TRUE);
        MoveWindow(cancel_, cancelX, y, btnW, btnH, TRUE);
        y = S(44);
        MoveWindow(lblIn_, pad, y + S(4), S(kLabelW) - S(4), S(18), TRUE);
        PlaceBox({editX, y, rc.right - pad, y + rowH}, in_, 1);
        y = S(78);
        int x = editX;
        for (int i = 0; i < 3; ++i) {
            PlaceCheck(i, x, y);
            x = checkRight_[i] + S(18);
        }
        y = S(106);
        PlaceCheck(3, editX, y);
        const int maskLeft = checkRight_[3] + S(8);
        PlaceBox({maskLeft, y, (std::min)(static_cast<int>(rc.right) - pad, maskLeft + S(360)), y + rowH}, mask_, 2);
        y = S(134);
        PlaceCheck(4, editX, y);
        const int listTop = S(168);
        const int buttonsY = rc.bottom - pad - btnH;
        const int statusY = buttonsY - S(26);
        MoveWindow(statusLabel_, pad, statusY, rc.right - 2 * pad, S(20), TRUE);
        MoveWindow(list_, pad, listTop, rc.right - 2 * pad, (std::max)(S(40), statusY - S(6) - listTop), TRUE);
        MoveWindow(close_, rc.right - pad - btnW, buttonsY, btnW, btnH, TRUE);
        MoveWindow(copy_, rc.right - pad - 2 * btnW - S(8), buttonsY, btnW, btnH, TRUE);
        FitColumns();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // The Text column takes what the other two leave.
    void FitColumns() {
        if (!list_) return;
        RECT lr;
        GetClientRect(list_, &lr);
        const int used = ListView_GetColumnWidth(list_, 0) + ListView_GetColumnWidth(list_, 1);
        ListView_SetColumnWidth(list_, 2, (std::max)(S(120), static_cast<int>(lr.right) - used));
    }

    void MakeColumns() {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        col.cx = S(280);
        col.pszText = const_cast<wchar_t*>(L"File");
        ListView_InsertColumn(list_, 0, &col);
        col.fmt = LVCFMT_RIGHT;
        col.cx = S(60);
        col.pszText = const_cast<wchar_t*>(L"Line");
        ListView_InsertColumn(list_, 1, &col);
        col.fmt = LVCFMT_LEFT;
        col.cx = S(400);
        col.pszText = const_cast<wchar_t*>(L"Text");
        ListView_InsertColumn(list_, 2, &col);
    }

    // ---- fonts, theme ----

    void ApplyDpi(UINT dpi) {
        dpi_ = dpi;
        HFONT oldFont = font_, oldMono = mono_;
        font_ = MessageFont(dpi_);
        mono_ = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        for (HWND h : {lblFind_, lblIn_, find_, in_, mask_, searchBtn_, cancel_, copy_, close_, statusLabel_, list_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        for (int i = 0; i < 5; ++i) SendMessageW(labels_[i], WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (oldFont) DeleteObject(oldFont);
        if (oldMono) DeleteObject(oldMono);
    }

    void ApplyTheme() {
        theme_ = CurrentTheme();
        contrast_ = theme_.text == theme_.faint;  // high contrast: one text colour
        for (HBRUSH* b : {&windowBrush_, &inputBrush_})
            if (*b) DeleteObject(*b);
        windowBrush_ = CreateSolidBrush(theme_.window);
        inputBrush_ = CreateSolidBrush(theme_.inputBack);
        BOOL dark = theme_.dark;
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        const wchar_t* theme = theme_.dark ? L"DarkMode_Explorer" : L"Explorer";
        for (HWND h : {list_, searchBtn_, cancel_, copy_, close_, find_, in_, mask_}) SetWindowTheme(h, theme, nullptr);
        for (int i = 0; i < 5; ++i) SetWindowTheme(checks_[i], theme, nullptr);
        if (HWND header = ListView_GetHeader(list_)) SetWindowTheme(header, theme_.dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
        ListView_SetBkColor(list_, theme_.inputBack);
        ListView_SetTextBkColor(list_, theme_.inputBack);
        ListView_SetTextColor(list_, theme_.text);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    // ---- options ----

    bool Checked(int slot) const { return SendMessageW(checks_[slot], BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void SetChecked(int slot, bool on) { SendMessageW(checks_[slot], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

    SearchWindowOptions CurrentOptions() const {
        SearchWindowOptions o;
        o.matchCase = Checked(0);
        o.wholeWord = Checked(1);
        o.regex = Checked(2);
        o.otherText = Checked(3);
        o.extensions = WindowText(mask_);
        o.decompilePex = Checked(4);
        return o;
    }

    void ApplyOptions(const SearchWindowOptions& o) {
        SetChecked(0, o.matchCase);
        SetChecked(1, o.wholeWord);
        SetChecked(2, o.regex);
        SetChecked(3, o.otherText);
        SetChecked(4, o.decompilePex);
        SetWindowTextW(mask_, (o.extensions.empty() ? DefaultExtraExtensionsText() : o.extensions).c_str());
        EnableWindow(mask_, o.otherText);
    }

    // ---- searching ----

    const TextSearchFile& FileOf(const TextSearchHit& h) const {
        static const TextSearchFile none;
        return h.file < files_.size() ? files_[h.file] : none;
    }

    std::wstring ResultsText() const {
        std::wstring text = L"Search \"" + lastQuery_ + L"\" in " + lastScope_ + L"\r\n";
        for (const TextSearchHit& h : hits_) text += FileOf(h).display + L":" + std::to_wstring(h.line) + L":" + h.text + L"\r\n";
        return text;
    }

    void SetStatus(const std::wstring& text, bool bad = false) {
        if (text == status_ && bad == statusBad_) return;
        status_ = text;
        statusBad_ = bad;
        SetWindowTextW(statusLabel_, text.c_str());
        InvalidateRect(statusLabel_, nullptr, TRUE);
    }

    // The status a finished (or failed) search leaves; an open's message is put back to it.
    void SetResultStatus(const std::wstring& text, bool bad) {
        resultStatus_ = text;
        resultBad_ = bad;
        SetStatus(text, bad);
    }

    void SetRunning(bool running) {
        running_ = running;
        EnableWindow(cancel_, running);
        EnableWindow(copy_, !running && !hits_.empty());
    }

    void StartSearch() {
        const std::wstring query = WindowText(find_);
        if (query.empty()) {
            SetStatus(L"Type what to find.");
            SetFocus(find_);
            return;
        }
        std::wstring scope = Trim(WindowText(in_));
        if (scope.size() >= 2 && scope.front() == L'"' && scope.back() == L'"') scope = Trim(scope.substr(1, scope.size() - 2));
        while (scope.size() > 3 && (scope.back() == L'\\' || scope.back() == L'/')) scope.pop_back();
        // A new search replaces the old one: stop it first (the destructor cancels and waits).
        KillTimer(hwnd_, kTimerTick);
        search_.reset();
        hits_.clear();
        files_.clear();
        ListView_SetItemCountEx(list_, 0, 0);
        FitColumns();
        InvalidateRect(list_, nullptr, TRUE);
        SetRunning(false);

        const SearchWindowOptions chosen = CurrentOptions();
        if (png_.empty()) SaveSearchOptions(chosen);
        TextSearchOptions o;
        o.query = query;
        o.matchCase = chosen.matchCase;
        o.wholeWord = chosen.wholeWord;
        o.regex = chosen.regex;
        o.otherText = chosen.otherText;
        o.extraExtensions = ParseExtensionList(chosen.extensions.empty() ? DefaultExtraExtensionsText() : chosen.extensions);
        o.decompilePex = chosen.decompilePex;
        maxHits_ = o.maxHits;
        lastQuery_ = query;
        lastScope_ = scope;
        const std::wstring title = L"Search text - " + ScopeTitleName(scope);
        SetWindowTextW(hwnd_, title.c_str());

        search_ = std::make_unique<TextSearch>();
        if (!search_->Start(scope, o)) {
            const std::wstring error = search_->Stats().firstError;
            SetResultStatus(error.empty() ? L"The search couldn't start." : error, true);
            search_.reset();
            if (!png_.empty()) PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
            return;
        }
        SetRunning(true);
        UpdateRunningStatus(search_->Stats());
        SetTimer(hwnd_, kTimerTick, kTickMs, nullptr);
    }

    void CancelSearch() {
        if (search_ && running_) {
            search_->Cancel();
            EnableWindow(cancel_, FALSE);
        }
    }

    void UpdateRunningStatus(const TextSearchStats& s) {
        resultStatus_ = L"Searching… " + Count(s.filesSearched, L"file", L"files") + L", " + Count(s.archivesOpened, L"archive", L"archives") + L" · " +
                        Count(s.hits, L"matching line", L"matching lines") + L" in " + Count(s.filesWithHits, L"file", L"files") + L" · " +
                        OneDecimal(s.seconds) + L" s";
        resultBad_ = false;
        SetStatus(resultStatus_);
    }

    void TakeNewHits() {
        const size_t before = hits_.size();
        search_->TakeHits(hits_);
        for (size_t i = before; i < hits_.size(); ++i)
            while (files_.size() <= hits_[i].file) files_.push_back(search_->File(static_cast<uint32_t>(files_.size())));
    }

    void OnTick() {
        if (!search_) return;
        const TextSearchStats s = search_->Stats();  // read before the hits: done means every hit is already there
        TakeNewHits();
        if (!s.done) {
            if (hits_.size() != static_cast<size_t>(ListView_GetItemCount(list_))) {
                ListView_SetItemCountEx(list_, static_cast<int>(hits_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
                FitColumns();  // the vertical scroll bar may have appeared
            }
            UpdateRunningStatus(s);
            return;
        }
        KillTimer(hwnd_, kTimerTick);
        SortHits();
        const TextSearchStats f = search_->Stats();
        SetRunning(false);
        SetResultStatus(FinalStatus(f), !f.firstError.empty() && f.errors == 0);
        if (!png_.empty()) PostMessageW(hwnd_, WM_APP_SNAP, 0, 0);
    }

    std::wstring FinalStatus(const TextSearchStats& s) const {
        std::wstring t;
        if (s.cancelled) t = L"Cancelled · ";
        t += Count(s.hits, L"matching line", L"matching lines") + L" in " + Count(s.filesWithHits, L"file", L"files") + L" · searched " +
             Count(s.filesSearched, L"file", L"files") + L" (" + OneDecimal(static_cast<double>(s.bytesSearched) / (1024.0 * 1024.0)) + L" MB) and " +
             Count(s.archivesOpened, L"archive", L"archives");
        if (s.pexDecompiled) t += L", " + Count(s.pexDecompiled, L"script", L"scripts") + L" decompiled";
        t += L" · " + OneDecimal(s.seconds) + L" s";
        if (s.truncated) t += L" · Stopped at " + Num(maxHits_) + L" lines";
        if (s.errors) {
            t += L" · " + Num(s.errors) + L" couldn't be read";
            if (!s.firstError.empty()) t += L" (first: " + s.firstError + L")";
        } else if (!s.firstError.empty()) {
            t += L" · " + s.firstError;
        }
        return t;
    }

    // By file path (case-insensitive), then line. Keeps the selected row selected.
    void SortHits() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        uint32_t keyFile = 0, keyLine = 0;
        const bool hadSelection = selected >= 0 && static_cast<size_t>(selected) < hits_.size();
        if (hadSelection) {
            keyFile = hits_[static_cast<size_t>(selected)].file;
            keyLine = hits_[static_cast<size_t>(selected)].line;
        }
        std::vector<uint32_t> order(files_.size()), rank(files_.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const int c = CompareStringOrdinal(files_[a].display.c_str(), -1, files_[b].display.c_str(), -1, TRUE);
            return c != CSTR_EQUAL ? c == CSTR_LESS_THAN : a < b;
        });
        for (size_t i = 0; i < order.size(); ++i) rank[order[i]] = static_cast<uint32_t>(i);
        std::stable_sort(hits_.begin(), hits_.end(), [&](const TextSearchHit& a, const TextSearchHit& b) {
            return a.file != b.file ? rank[a.file] < rank[b.file] : a.line < b.line;
        });
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemCountEx(list_, static_cast<int>(hits_.size()), 0);
        FitColumns();
        if (hadSelection) {
            for (size_t i = 0; i < hits_.size(); ++i) {
                if (hits_[i].file == keyFile && hits_[i].line == keyLine) {
                    ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(list_, static_cast<int>(i), FALSE);
                    break;
                }
            }
        }
        InvalidateRect(list_, nullptr, TRUE);
    }

    // ---- rows ----

    int SelectedRow() const {
        const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        return row >= 0 && static_cast<size_t>(row) < hits_.size() ? row : -1;
    }

    void OpenRow(int row) {
        if (row < 0 || static_cast<size_t>(row) >= hits_.size()) return;
        const TextSearchHit& h = hits_[static_cast<size_t>(row)];
        HMODULE module = AddModuleRef();
        auto* args = new OpenArgs{FileOf(h), h.line, hwnd_, openState_, module};
        if (!StartThread(&OpenThread, args, module)) {
            delete args;
            SetStatus(L"Couldn't start the editor.", true);
            return;
        }
        SetStatus(L"Opening " + FileOf(h).display + L"…");
    }

    std::wstring RowPath(int row) const {
        const TextSearchFile& f = FileOf(hits_[static_cast<size_t>(row)]);
        if (f.inner.empty()) return f.diskPath;
        std::wstring inner = FromBytes(f.inner);
        std::replace(inner.begin(), inner.end(), L'/', L'\\');
        return f.diskPath + L"\\" + inner;
    }

    void CopyText(const std::wstring& text) {
        if (!SetClipboardText(hwnd_, text)) SetStatus(L"The clipboard couldn't be opened.", true);
    }

    void CopyResults() {
        if (hits_.empty() || running_) return;
        if (!SetClipboardText(hwnd_, ResultsText())) return;
        SetWindowTextW(copy_, L"Copied");
        SetTimer(hwnd_, kTimerCopied, 1500, nullptr);
    }

    void SelectRow(int row) {
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    void ShowRowMenu(int row, POINT screen) {
        SelectRow(row);
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open");
        AppendMenuW(menu, MF_STRING, IDM_COPYPATH, L"Copy path");
        AppendMenuW(menu, MF_STRING, IDM_COPYLINE, L"Copy line");
        SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (row < 0 || static_cast<size_t>(row) >= hits_.size()) return;
        if (cmd == IDM_OPEN) OpenRow(row);
        else if (cmd == IDM_COPYPATH) CopyText(RowPath(row));
        else if (cmd == IDM_COPYLINE) CopyText(hits_[static_cast<size_t>(row)].text);
    }

    void OnContextMenu(HWND from, LPARAM lp) {
        if (from != list_) return;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int row;
        if (pt.x == -1 && pt.y == -1) {  // the keyboard
            row = SelectedRow();
            if (row < 0) return;
            RECT r;
            ListView_GetItemRect(list_, row, &r, LVIR_BOUNDS);
            pt = {r.left + S(20), r.bottom};
            ClientToScreen(list_, &pt);
        } else {
            POINT c = pt;
            ScreenToClient(list_, &c);
            LVHITTESTINFO hit{};
            hit.pt = c;
            row = ListView_HitTest(list_, &hit);
            if (row < 0 || static_cast<size_t>(row) >= hits_.size()) return;
        }
        ShowRowMenu(row, pt);
    }

    // ---- list drawing ----

    void FillDispInfo(NMLVDISPINFOW* di) {
        LVITEMW& it = di->item;
        if (!(it.mask & LVIF_TEXT) || it.iItem < 0 || static_cast<size_t>(it.iItem) >= hits_.size() || !it.pszText || it.cchTextMax <= 0) return;
        const TextSearchHit& h = hits_[static_cast<size_t>(it.iItem)];
        std::wstring text;
        if (it.iSubItem == 0) text = FileOf(h).display;
        else if (it.iSubItem == 1) text = std::to_wstring(h.line);
        else text = h.text;
        wcsncpy_s(it.pszText, static_cast<size_t>(it.cchTextMax), text.c_str(), _TRUNCATE);
    }

    void SelectionColors(COLORREF& back, COLORREF& fore) const {
        const bool focus = GetFocus() == list_;
        if (theme_.dark) {
            back = focus ? RGB(38, 79, 120) : RGB(62, 62, 62);
            fore = RGB(255, 255, 255);
        } else {
            back = GetSysColor(focus ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
            fore = GetSysColor(focus ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
        }
    }

    // Every cell is drawn here (not only the Text one), so the selection looks the same across the row.
    void DrawCell(NMLVCUSTOMDRAW* cd) {
        HDC dc = cd->nmcd.hdc;
        const int row = static_cast<int>(cd->nmcd.dwItemSpec), sub = cd->iSubItem;
        RECT r{};
        ListView_GetSubItemRect(list_, row, sub, LVIR_BOUNDS, &r);
        if (sub == 0) r.right = r.left + ListView_GetColumnWidth(list_, 0);  // for item 0 the bounds span the row
        const bool selected = (ListView_GetItemState(list_, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        COLORREF back = theme_.inputBack, fore = theme_.text;
        if (selected) SelectionColors(back, fore);
        FillBox(dc, r, back);
        if (row < 0 || static_cast<size_t>(row) >= hits_.size()) return;
        const TextSearchHit& h = hits_[static_cast<size_t>(row)];
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fore);
        RECT t = r;
        if (sub == 0) {
            t.left += S(6);
            t.right -= S(4);
            DrawTextW(dc, FileOf(h).display.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        } else if (sub == 1) {
            t.right -= S(8);
            const std::wstring n = std::to_wstring(h.line);
            DrawTextW(dc, n.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
        } else {
            DrawMatchText(dc, r, h, selected, fore);
        }
    }

    void DrawMatchText(HDC dc, const RECT& r, const TextSearchHit& h, bool selected, COLORREF fore) {
        std::wstring text;
        Ranges ranges;
        ExpandTabs(h.text, h.ranges, text, ranges);
        HGDIOBJ oldFont = SelectObject(dc, mono_);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        const int ty = r.top + ((r.bottom - r.top) - tm.tmHeight) / 2;
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
        int x = r.left + S(6);
        const COLORREF hitText = contrast_ ? GetSysColor(COLOR_HIGHLIGHTTEXT) : theme_.text;
        auto draw = [&](size_t a, size_t b, bool hit) {
            if (b <= a || x >= r.right) return;
            SIZE sz{};
            GetTextExtentPoint32W(dc, text.c_str() + a, static_cast<int>(b - a), &sz);
            if (hit && !selected) {
                RECT box{x, r.top + 1, x + sz.cx, r.bottom - 1};
                FillBox(dc, box, theme_.matchBack);
                SetTextColor(dc, hitText);
            } else {
                SetTextColor(dc, fore);
            }
            ExtTextOutW(dc, x, ty, 0, nullptr, text.c_str() + a, static_cast<UINT>(b - a), nullptr);
            x += sz.cx;
        };
        size_t pos = 0;
        for (const auto& range : ranges) {
            const size_t a = (std::max)(pos, static_cast<size_t>(range.first)), b = (std::min)(text.size(), static_cast<size_t>(range.second));
            if (b <= a) continue;
            draw(pos, a, false);
            draw(a, b, true);
            pos = b;
        }
        draw(pos, text.size(), false);
        RestoreDC(dc, saved);
        SelectObject(dc, oldFont);
    }

    LRESULT OnNotify(NMHDR* nm) {
        if (nm->hwndFrom != list_) return 0;
        switch (nm->code) {
            case LVN_GETDISPINFOW:
                FillDispInfo(reinterpret_cast<NMLVDISPINFOW*>(nm));
                return 0;
            case NM_CUSTOMDRAW: {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
                    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: DrawCell(cd); return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            case NM_DBLCLK: {
                const auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
                OpenRow(ia->iItem);
                return 0;
            }
        }
        return 0;
    }

    // ---- the window ----

    void Paint(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillBox(dc, rc, theme_.window);
        for (int i = 0; i < 3; ++i) {
            const RECT& b = boxes_[i];
            FillBox(dc, b, theme_.inputBack);
            HBRUSH line = CreateSolidBrush(theme_.border);
            FrameRect(dc, &b, line);
            DeleteObject(line);
        }
    }

    void OnOpened() {
        std::wstring error;
        {
            std::lock_guard<std::mutex> g(openState_->lock);
            error.swap(openState_->error);
        }
        if (!error.empty()) SetStatus(error, true);
        else SetStatus(resultStatus_, resultBad_);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: return OnCreate();
            case WM_APP_OPENED: OnOpened(); return 0;
            case WM_APP_FIT: FitColumns(); return 0;
            case WM_APP_SNAP: Snapshot(); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                Paint(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wp)); return 0;
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);
                HWND h = reinterpret_cast<HWND>(lp);
                if (h == find_ || h == in_ || h == mask_) {
                    SetTextColor(dc, IsWindowEnabled(h) ? theme_.text : theme_.faint);
                    SetBkColor(dc, theme_.inputBack);
                    return reinterpret_cast<LRESULT>(inputBrush_);
                }
                SetTextColor(dc, h == statusLabel_ ? (statusBad_ ? theme_.badLine : theme_.faint) : theme_.text);
                SetBkColor(dc, theme_.window);
                return reinterpret_cast<LRESULT>(windowBrush_);
            }
            case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(windowBrush_);
            case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lp));
            case WM_CONTEXTMENU: OnContextMenu(reinterpret_cast<HWND>(wp), lp); return 0;
            case WM_TIMER:
                if (wp == kTimerTick) {
                    OnTick();
                } else if (wp == kTimerCopied) {
                    KillTimer(hwnd_, kTimerCopied);
                    SetWindowTextW(copy_, L"Copy results");
                }
                return 0;
            case WM_COMMAND: OnCommand(LOWORD(wp), HIWORD(wp)); return 0;
            case WM_SETFOCUS:
                SetFocus(find_);
                return 0;
            case WM_SIZE:
                if (list_ && wp != SIZE_MINIMIZED) Layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize = {S(560), S(400)};
                return 0;
            }
            case WM_DPICHANGED: {
                ApplyDpi(HIWORD(wp));
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                ApplyTheme();
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd_, kTimerTick);
                KillTimer(hwnd_, kTimerCopied);
                if (png_.empty() && list_) SaveSearchOptions(CurrentOptions());
                search_.reset();  // cancels and waits
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                for (HFONT f : {font_, mono_})
                    if (f) DeleteObject(f);
                for (HBRUSH b : {windowBrush_, inputBrush_})
                    if (b) DeleteObject(b);
                font_ = mono_ = nullptr;
                windowBrush_ = inputBrush_ = nullptr;
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_, CtrlId(id), g_hInst, nullptr);
    }

    LRESULT OnCreate() {
        dpi_ = GetDpiForWindow(hwnd_);
        if (!png_.empty()) {
            // Tests: PSE_DPI=144 draws the window at 150% on any screen.
            wchar_t forced[16] = {};
            if (GetEnvironmentVariableW(L"PSE_DPI", forced, ARRAYSIZE(forced))) {
                const int v = _wtoi(forced);
                if (v >= 96 && v <= 384) dpi_ = static_cast<UINT>(v);
            }
        }
        const DWORD tab = WS_TABSTOP;
        lblFind_ = Child(L"STATIC", L"Find:", SS_LEFT | SS_NOPREFIX, IDC_LBL_FIND);
        find_ = Child(L"EDIT", L"", tab | ES_AUTOHSCROLL, IDC_FIND);
        lblIn_ = Child(L"STATIC", L"In:", SS_LEFT | SS_NOPREFIX, IDC_LBL_IN);
        in_ = Child(L"EDIT", scope_.c_str(), tab | ES_AUTOHSCROLL, IDC_IN);
        static const wchar_t* const kLabels[5] = {L"Match case", L"Whole word", L"Regex", L"Other text files:", L"Decompile .pex that have no source"};
        static const int kIds[5] = {IDC_MATCHCASE, IDC_WHOLEWORD, IDC_REGEX, IDC_OTHER, IDC_DECOMP};
        for (int i = 0; i < 5; ++i) {
            // The label is a separate control: a themed checkbox draws black text in dark mode.
            checks_[i] = Child(L"BUTTON", L"", tab | BS_AUTOCHECKBOX, kIds[i]);
            labels_[i] = Child(L"STATIC", kLabels[i], SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE, IDC_LABEL_BASE + i);
            if (i == 3) mask_ = Child(L"EDIT", L"", tab | ES_AUTOHSCROLL, IDC_MASK);
        }
        searchBtn_ = Child(L"BUTTON", L"Search", tab | BS_DEFPUSHBUTTON, IDOK);
        cancel_ = Child(L"BUTTON", L"Cancel", tab | WS_DISABLED, IDC_CANCELSEARCH);
        list_ = Child(L"SysListView32", L"", tab | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, IDC_RESULTS);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        statusLabel_ = Child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);
        copy_ = Child(L"BUTTON", L"Copy results", tab | WS_DISABLED, IDC_COPYRES);
        close_ = Child(L"BUTTON", L"Close", tab, IDC_CLOSEBTN);
        MakeColumns();
        SetWindowSubclass(list_, &SearchWindow::ListProc, 1, reinterpret_cast<DWORD_PTR>(this));
        ApplyDpi(dpi_);
        ApplyTheme();
        // Tests start from the defaults, unless PSE_SEARCH_OPTIONS_FILE names an options file to use (never written in a test).
        ApplyOptions(png_.empty() || GetEnvironmentVariableW(L"PSE_SEARCH_OPTIONS_FILE", nullptr, 0) ? LoadSearchOptions() : SearchWindowOptions{});
        RECT r{0, 0, S(kWidth), S(kHeight)};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)), dpi_);
        CenterOnCursorMonitor(hwnd_, r.right - r.left, r.bottom - r.top);
        Layout();
        if (png_.empty()) {
            // Old copies of opened archive entries go, off this thread.
            HMODULE module = AddModuleRef();
            StartThread(&CleanUpThread, module, module);
        } else {
            SetWindowTextW(find_, query_.c_str());
            StartSearch();
        }
        return 0;
    }

    void OnCommand(int id, int code) {
        switch (id) {
            case IDOK:  // Enter: on a row it opens it, anywhere else it searches
                if (GetFocus() == list_ && SelectedRow() >= 0) OpenRow(SelectedRow());
                else StartSearch();
                return;
            case IDCANCEL:  // Esc
                if (running_) CancelSearch();
                else DestroyWindow(hwnd_);
                return;
            case IDC_CANCELSEARCH: CancelSearch(); return;
            case IDC_COPYRES: CopyResults(); return;
            case IDC_CLOSEBTN: DestroyWindow(hwnd_); return;
            case IDC_OTHER:
                if (code == BN_CLICKED) EnableWindow(mask_, Checked(3));
                return;
        }
        if (id >= IDC_LABEL_BASE && id < IDC_LABEL_BASE + 5 && code == STN_CLICKED) {
            const int slot = id - IDC_LABEL_BASE;
            SetChecked(slot, !Checked(slot));
            SetFocus(checks_[slot]);
            if (slot == 3) EnableWindow(mask_, Checked(3));
        }
    }

    // Tests: the search is done; save the picture, the status line and the rows, and close.
    void Snapshot() {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        SaveWindowPng(hwnd_, png_, nullptr, nullptr);
        const std::wstring text = L"status\t" + status_ + L"\r\nrows\t" + std::to_wstring(hits_.size()) + L"\r\n" + ResultsText();
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
        HANDLE f = CreateFileW((png_ + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
            CloseHandle(f);
        }
        DestroyWindow(hwnd_);
    }

    std::wstring scope_, query_, png_, status_, resultStatus_, lastQuery_, lastScope_;
    bool statusBad_ = false, resultBad_ = false, running_ = false, contrast_ = false;
    size_t maxHits_ = 0;
    HWND hwnd_ = nullptr, lblFind_ = nullptr, lblIn_ = nullptr, find_ = nullptr, in_ = nullptr, mask_ = nullptr, searchBtn_ = nullptr, cancel_ = nullptr;
    HWND list_ = nullptr, statusLabel_ = nullptr, copy_ = nullptr, close_ = nullptr;
    HWND checks_[5] = {}, labels_[5] = {};
    int checkRight_[5] = {};
    RECT boxes_[3] = {};
    UINT dpi_ = 96;
    HFONT font_ = nullptr, mono_ = nullptr;
    HBRUSH windowBrush_ = nullptr, inputBrush_ = nullptr;
    Theme theme_ = CurrentTheme();
    std::unique_ptr<TextSearch> search_;
    std::vector<TextSearchHit> hits_;
    std::vector<TextSearchFile> files_;
    std::shared_ptr<OpenState> openState_ = std::make_shared<OpenState>();
};

std::mutex g_searchClassLock;
int g_searchUsers = 0;

struct SearchThreadArgs {
    std::wstring scope;
    HMODULE module;
};

DWORD WINAPI SearchThread(void* param) {
    std::unique_ptr<SearchThreadArgs> a(static_cast<SearchThreadArgs*>(param));
    try {
        RunSearchWindow(a->scope, L"", L"");
    } catch (...) {
    }
    HMODULE module = a->module;
    a.reset();
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

int RunSearchWindow(const std::wstring& scope, const std::wstring& query, const std::wstring& png) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    {
        ActivationContext act;
        DpiScope dpi;
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icc);
        {
            std::lock_guard<std::mutex> g(g_searchClassLock);
            if (g_searchUsers++ == 0) SearchWindow::Register();
        }
        {
            SearchWindow window(scope, query, png);
            if (HWND hwnd = window.Create()) {
                ShowWindow(hwnd, png.empty() ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
                if (png.empty()) SetForegroundWindow(hwnd);
                result = RunLoop(hwnd);
            }
        }
        std::lock_guard<std::mutex> g(g_searchClassLock);
        if (--g_searchUsers == 0) UnregisterClassW(kSearchClass, g_hInst);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return result;
}

void ShowSearchWindowAsync(const std::wstring& scope) {
    try {
        HMODULE module = AddModuleRef();
        auto* args = new SearchThreadArgs{scope, module};
        if (!StartThread(&SearchThread, args, module)) delete args;
    } catch (...) {
    }
}

}  // namespace pse
