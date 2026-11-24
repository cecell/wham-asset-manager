// Search text: see text_search.h. Workers pull folders, loose files and
// archives from one queue; each file is read in chunks (or each archive entry
// into memory) and handed to a Scanner that finds matching lines.
//
// Lines: '\n' ends a line, a trailing '\r' is dropped. Regex runs on one line at
// a time, so a pattern can't match across lines and ^ / $ are line anchors.
// A line longer than kMaxLine (4 MB) is searched only in its first 4 MB.
#include "text_search.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_set>

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#ifndef PCRE2_STATIC
#define PCRE2_STATIC 1
#endif
#include <pcre2.h>

#include <bsa/reader.hpp>

#include "bsa_archive.h"
#include "papyrus_tools.h"
#include "work_budget.h"

namespace pse {
namespace fs = std::filesystem;

namespace {

constexpr size_t kChunk = 4u << 20;           // bytes read from a loose file at a time
constexpr size_t kMaxLine = 4u << 20;         // longer lines are searched only in their first part
constexpr size_t kMaxUtf16 = 64u << 20;       // UTF-16 files are converted whole
constexpr size_t kMaxEntry = 64u << 20;       // archive entries bigger than this are skipped
constexpr size_t kShowChars = 1000;           // displayed text is cut to about this
constexpr size_t kMaxRanges = 4096;           // hit ranges kept per line
constexpr size_t kSniff = 4096;               // bytes checked for NULs
constexpr size_t kPexBatch = 256;             // archive .pex per phase 2 item
constexpr uint64_t kCopyMaxAgeSeconds = 24 * 3600;

using Span = std::pair<size_t, size_t>;

// ---------------------------------------------------------------- strings

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Windows-1252 bytes of `w`; false when some character has no 1252 form.
bool To1252(const std::wstring& w, std::string& out) {
    if (w.empty()) return false;
    BOOL used = FALSE;
    const int n = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, &used);
    if (n <= 0 || used) return false;
    out.assign(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, &used);
    return !used;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

std::string LowerAsciiCopy(std::string s) {
    for (char& c : s) c = LowerAscii(c);
    return s;
}

void FoldAscii(const char* s, char* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned c = static_cast<unsigned char>(s[i]);
        d[i] = static_cast<char>(c + ((c - 'A') < 26u ? 32u : 0u));
    }
}

bool IsWordByte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80; }

bool ValidUtf8(const unsigned char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return false;
        if (len > n - i) return false;
        for (size_t k = 1; k < len; ++k)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        if ((c == 0xE0 && s[i + 1] < 0xA0) || (c == 0xED && s[i + 1] >= 0xA0) || (c == 0xF0 && s[i + 1] < 0x90) || (c == 0xF4 && s[i + 1] >= 0x90)) return false;
        i += len;
    }
    return true;
}

std::wstring WindowsError(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message : L"error " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

// Past MAX_PATH the \\?\ form is needed.
std::wstring LongPath(const std::wstring& p) {
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 3 && p[1] == L':') return L"\\\\?\\" + p;
    return p;
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

// Display names use backslashes whatever the archive stored.
std::wstring BackslashPath(std::wstring s) {
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : Lower(name.substr(dot));
}

std::string ExtensionOfNarrow(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    return LowerAsciiCopy(std::string(name.substr(dot)));
}

bool IsSkippedFolder(const std::wstring& name) {
    static const wchar_t* const names[] = {L"meshes", L"textures", L"sound", L"music", L"video", L"strings", L"seq",
                                           L"grass", L"lodsettings", L"shadersfx", L"interface"};
    const std::wstring lower = Lower(name);
    for (const wchar_t* n : names)
        if (lower == n) return true;
    return false;
}

// ---------------------------------------------------------------- RequiredLiteral

bool IsDigitC(char c) { return c >= '0' && c <= '9'; }
bool IsAlnumC(char c) { return IsDigitC(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Length of the quantifier at p[i] (with its lazy or possessive suffix), 0 if
// none; `minRep` is the least number of repeats it allows.
size_t QuantifierLength(const std::string& p, size_t i, int& minRep) {
    if (i >= p.size()) return 0;
    size_t j;
    const char c = p[i];
    if (c == '?' || c == '*') {
        minRep = 0;
        j = i + 1;
    } else if (c == '+') {
        minRep = 1;
        j = i + 1;
    } else if (c == '{') {
        size_t k = i + 1;
        const size_t d1 = k;
        int n = 0;
        while (k < p.size() && IsDigitC(p[k])) {
            n = std::min(n * 10 + (p[k] - '0'), 100000);
            ++k;
        }
        const bool hasMin = k > d1;
        bool ok = false;
        if (k < p.size() && p[k] == '}' && hasMin) {
            ok = true;
        } else if (k < p.size() && p[k] == ',') {
            ++k;
            const size_t d2 = k;
            while (k < p.size() && IsDigitC(p[k])) ++k;
            if (k < p.size() && p[k] == '}' && (hasMin || k > d2)) ok = true;
        }
        if (!ok) return 0;
        minRep = hasMin ? n : 0;
        j = k + 1;
    } else {
        return 0;
    }
    if (j < p.size() && (p[j] == '?' || p[j] == '+')) ++j;
    return j - i;
}

// p[i] is '['. Returns the index after the closing ']'.
size_t SkipClass(const std::string& p, size_t i) {
    size_t j = i + 1;
    if (j < p.size() && p[j] == '^') ++j;
    if (j < p.size() && p[j] == ']') ++j;
    while (j < p.size()) {
        if (p[j] == '\\') {
            j += 2;
        } else if (p[j] == '[' && j + 1 < p.size() && (p[j + 1] == ':' || p[j + 1] == '.' || p[j + 1] == '=')) {
            const std::string close = std::string(1, p[j + 1]) + "]";
            const size_t e = p.find(close, j + 2);
            j = e == std::string::npos ? j + 1 : e + 2;
        } else if (p[j] == ']') {
            return j + 1;
        } else {
            ++j;
        }
    }
    return p.size();
}

// p[i] is '('. Returns the index after the matching ')'.
size_t SkipGroup(const std::string& p, size_t i) {
    int depth = 0;
    size_t j = i;
    while (j < p.size()) {
        const char c = p[j];
        if (c == '\\') {
            j += 2;
        } else if (c == '[') {
            j = SkipClass(p, j);
        } else if (c == '(') {
            ++depth;
            ++j;
        } else if (c == ')') {
            ++j;
            if (--depth == 0) return j;
        } else {
            ++j;
        }
    }
    return p.size();
}

size_t SkipTo(const std::string& p, size_t i, char close) {
    const size_t e = p.find(close, i);
    return e == std::string::npos ? p.size() : e + 1;
}

// Extended mode ignores whitespace and # comments, so no literal is safe.
bool HasExtendedFlag(const std::string& p) {
    for (size_t i = p.find("(?"); i != std::string::npos; i = p.find("(?", i + 1)) {
        for (size_t j = i + 2; j < p.size() && (std::isalpha(static_cast<unsigned char>(p[j])) || p[j] == '^' || p[j] == '-'); ++j)
            if (p[j] == 'x') return true;
    }
    return false;
}

}  // namespace

std::string RequiredLiteral(const std::string& p) {
    if (HasExtendedFlag(p)) return {};
    std::string best, cur;
    auto flush = [&] {
        if (cur.size() > best.size()) best = cur;
        cur.clear();
    };
    auto skipQuantifier = [&](size_t& i) {
        int minRep = 0;
        i += QuantifierLength(p, i, minRep);
    };
    size_t i = 0;
    while (i < p.size()) {
        const char c = p[i];
        if (c == '|') return {};  // top-level alternation: nothing is required
        char literal = 0;
        bool isLiteral = false;
        if (c == '\\') {
            if (i + 1 >= p.size()) break;
            const char e = p[i + 1];
            if (static_cast<unsigned char>(e) < 0x80 && !IsAlnumC(e)) {
                literal = e;
                isLiteral = true;
                i += 2;
            } else {
                flush();
                i += 2;
                switch (e) {
                    case 'Q': {
                        const size_t end = p.find("\\E", i);
                        i = end == std::string::npos ? p.size() : end + 2;
                        break;
                    }
                    case 'x':
                        if (i < p.size() && p[i] == '{') i = SkipTo(p, i, '}');
                        else for (int k = 0; k < 2 && i < p.size() && std::isxdigit(static_cast<unsigned char>(p[i])); ++k) ++i;
                        break;
                    case 'p': case 'P': case 'N': case 'o':
                        if (i < p.size() && p[i] == '{') i = SkipTo(p, i, '}');
                        else if (e == 'p' || e == 'P') ++i;
                        break;
                    case 'g': case 'k':
                        if (i < p.size() && p[i] == '{') i = SkipTo(p, i, '}');
                        else if (i < p.size() && p[i] == '<') i = SkipTo(p, i, '>');
                        else if (i < p.size() && p[i] == '\'') i = SkipTo(p, i + 1, '\'');
                        else {
                            if (i < p.size() && (p[i] == '-' || p[i] == '+')) ++i;
                            while (i < p.size() && IsDigitC(p[i])) ++i;
                        }
                        break;
                    case 'c':
                        if (i < p.size()) ++i;
                        break;
                    default:
                        if (IsDigitC(e))
                            while (i < p.size() && IsDigitC(p[i])) ++i;
                        break;
                }
                skipQuantifier(i);
                continue;
            }
        } else if (c == '[') {
            flush();
            i = SkipClass(p, i);
            skipQuantifier(i);
            continue;
        } else if (c == '(') {
            flush();
            i = SkipGroup(p, i);
            skipQuantifier(i);
            continue;
        } else if (c == '.' || c == '^' || c == '$' || c == ')') {
            flush();
            ++i;
            skipQuantifier(i);
            continue;
        } else if (c == '*' || c == '+' || c == '?') {
            flush();  // a quantifier with nothing before it
            ++i;
            continue;
        } else if (c == '{') {
            int minRep = 0;
            const size_t q = QuantifierLength(p, i, minRep);
            if (q) {
                flush();
                i += q;
                continue;
            }
            literal = c;
            isLiteral = true;
            ++i;
        } else {
            literal = c;
            isLiteral = true;
            ++i;
        }
        if (isLiteral) {
            int minRep = 0;
            const size_t q = QuantifierLength(p, i, minRep);
            if (q == 0) {
                cur.push_back(literal);
            } else {
                if (minRep >= 1) cur.push_back(literal);
                flush();
                i += q;
            }
        }
    }
    flush();
    return best.size() >= 3 ? best : std::string();
}

std::wstring DefaultExtraExtensionsText() { return L".txt .ini .json .xml .yaml .toml .lst .csv"; }

std::vector<std::wstring> ParseExtensionList(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring token;
    auto add = [&] {
        size_t start = 0;
        while (start < token.size() && token[start] == L'*') ++start;
        std::wstring t = token.substr(start);
        token.clear();
        if (t.empty() || t == L".") return;
        if (t[0] != L'.') t.insert(t.begin(), L'.');
        t = Lower(t);
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    };
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L',' || c == L';' || c == L'\r' || c == L'\n') add();
        else token.push_back(c);
    }
    add();
    return out;
}

namespace {

// ---------------------------------------------------------------- query

struct RegexForm {
    pcre2_code* code = nullptr;
    std::string literal;  // RequiredLiteral of this form's pattern
};

struct Query {
    bool regex = false, caseless = true, wholeWord = false;
    std::vector<std::string> needles;  // literal mode: the forms to find (lower case when caseless)
    std::vector<RegexForm> forms;      // regex mode: UTF-8 form, and the 1252 form when it differs
    bool useLiterals = false;          // scan for literals first (always in literal mode)
    bool foldBuffer = false;           // the literal scan is case-insensitive
    std::vector<std::string> scan;     // what the literal scan looks for
    Query() = default;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    ~Query() {
        for (RegexForm& f : forms)
            if (f.code) pcre2_code_free(f.code);
    }
};

size_t CharOffset(const std::string& utf8, size_t byteOffset) {
    size_t n = 0;
    for (size_t i = 0; i < byteOffset && i < utf8.size(); ++i)
        if ((static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80) ++n;
    return n;
}

bool BuildQuery(const TextSearchOptions& o, Query& q, std::wstring& error) {
    q.regex = o.regex;
    q.caseless = !o.matchCase;
    q.wholeWord = o.wholeWord;
    const std::string u8 = ToUtf8(o.query);
    std::string cp;
    bool hasCp = false;
    if (std::any_of(o.query.begin(), o.query.end(), [](wchar_t c) { return c >= 0x80; })) hasCp = To1252(o.query, cp) && cp != u8;
    std::vector<std::string> forms{u8};
    if (hasCp) forms.push_back(cp);

    if (!o.regex) {
        if (o.query.find_first_of(L"\r\n") != std::wstring::npos) {
            error = L"The text to find can't contain a line break.";
            return false;
        }
        for (const std::string& f : forms) q.needles.push_back(q.caseless ? LowerAsciiCopy(f) : f);
        q.scan = q.needles;
        q.useLiterals = true;
        q.foldBuffer = q.caseless;
        return true;
    }

    const bool inlineFlags = u8.find("(?") != std::string::npos;
    q.foldBuffer = q.caseless || inlineFlags;
    q.useLiterals = true;
    for (size_t k = 0; k < forms.size(); ++k) {
        const std::string pattern = o.wholeWord ? "\\b(?:" + forms[k] + ")\\b" : forms[k];
        int code = 0;
        PCRE2_SIZE offset = 0;
        pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), q.caseless ? PCRE2_CASELESS : 0, &code, &offset, nullptr);
        if (!re) {
            if (k > 0) continue;  // the 1252 form is a bonus
            char message[256] = {};
            pcre2_get_error_message(code, reinterpret_cast<PCRE2_UCHAR*>(message), sizeof(message));
            size_t off = static_cast<size_t>(offset);
            if (o.wholeWord) off = off > 5 ? off - 5 : 0;
            off = std::min(off, forms[0].size());
            error = L"Bad regex at " + std::to_wstring(CharOffset(forms[0], off)) + L": " + Widen(message);
            return false;
        }
        RegexForm f;
        f.code = re;
        f.literal = RequiredLiteral(forms[k]);
        if (f.literal.empty()) q.useLiterals = false;
        else if (q.foldBuffer) f.literal = LowerAsciiCopy(f.literal);
        q.forms.push_back(std::move(f));
    }
    if (q.useLiterals)
        for (const RegexForm& f : q.forms) q.scan.push_back(f.literal);
    return true;
}

// ---------------------------------------------------------------- scanning

struct FileCtx {
    std::wstring display;
    std::wstring diskPath;
    std::string inner;
    bool decompiled = false;
    int64_t index = -1;  // into the engine's file list, once the first hit is found
};

class HitSink {
public:
    virtual ~HitSink() = default;
    virtual bool AddHit(FileCtx& file, uint32_t line, const char* text, size_t length, const std::vector<Span>& spans) = 0;
    virtual bool Stopped() const = 0;
};

size_t CountNewlines(const char* p, size_t n) { return static_cast<size_t>(std::count(p, p + n, '\n')); }

// Finds the matching lines of one file, fed in pieces.
class Scanner {
public:
    Scanner(const Query& q, HitSink& sink) : q_(q), sink_(sink) {
        searchers_.reserve(q.scan.size());
        for (const std::string& s : q.scan) searchers_.emplace_back(s.data(), s.data() + s.size());
        if (q.regex) {
            md_ = pcre2_match_data_create(4, nullptr);
            mc_ = pcre2_match_context_create(nullptr);
            if (!md_ || !mc_) throw std::bad_alloc();
            pcre2_set_match_limit(mc_, 1000000);
            pcre2_set_depth_limit(mc_, 1000000);
            pcre2_set_heap_limit(mc_, 20000);  // KiB
        }
    }
    ~Scanner() {
        if (md_) pcre2_match_data_free(md_);
        if (mc_) pcre2_match_context_free(mc_);
    }
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    void Begin(FileCtx* ctx) {
        ctx_ = ctx;
        lineNo_ = 1;
        carry_.clear();
        skipping_ = false;
    }

    void Feed(const char* d, size_t n) {
        for (size_t off = 0; off < n && !sink_.Stopped(); off += kChunk) FeedSlice(d + off, std::min(kChunk, n - off));
    }

    void End() {
        if (!carry_.empty() && !sink_.Stopped()) FlushCarry();
        carry_.clear();
    }

    // Big buffers aren't kept between files.
    void Trim() {
        if (foldCap_ > (1u << 20)) {
            fold_.reset();
            foldCap_ = 0;
        }
        if (carry_.capacity() > (1u << 20)) std::vector<char>().swap(carry_);
    }

private:
    struct Next {
        size_t s = 0, e = 0;
        bool has = false, dead = false;
    };

    void FlushCarry() {
        ProcessBuffer(carry_.data(), carry_.size(), lineNo_);
        ++lineNo_;
        carry_.clear();
    }

    void FeedSlice(const char* d, size_t n) {
        size_t pos = 0;
        if (skipping_) {  // the rest of an over-long line
            const void* nl = std::memchr(d, '\n', n);
            if (!nl) return;
            pos = static_cast<size_t>(static_cast<const char*>(nl) - d) + 1;
            skipping_ = false;
        }
        if (!carry_.empty()) {
            const char* nl = static_cast<const char*>(std::memchr(d + pos, '\n', n - pos));
            const size_t take = nl ? static_cast<size_t>(nl - (d + pos)) : n - pos;
            const size_t room = kMaxLine > carry_.size() ? kMaxLine - carry_.size() : 0;
            carry_.insert(carry_.end(), d + pos, d + pos + std::min(take, room));
            if (nl) {
                FlushCarry();
                pos = static_cast<size_t>(nl - d) + 1;
            } else if (carry_.size() >= kMaxLine) {
                FlushCarry();
                skipping_ = true;
                return;
            } else {
                return;
            }
        }
        const char* base = d + pos;
        const size_t len = n - pos;
        size_t last = 0;  // length up to and including the last '\n'
        for (size_t i = len; i > 0; --i) {
            if (base[i - 1] == '\n') {
                last = i;
                break;
            }
        }
        if (last) {
            ProcessBuffer(base, last, lineNo_);
            lineNo_ += static_cast<uint32_t>(CountNewlines(base, last));
        }
        const char* tail = base + last;
        const size_t tailLen = len - last;
        if (tailLen >= kMaxLine) {
            carry_.assign(tail, tail + kMaxLine);
            FlushCarry();
            skipping_ = true;
        } else if (tailLen) {
            carry_.assign(tail, tail + tailLen);
        }
    }

    bool WordOk(const char* b, size_t n, size_t s, size_t e) const {
        if (s > 0 && IsWordByte(static_cast<unsigned char>(b[s - 1]))) return false;
        if (e < n && IsWordByte(static_cast<unsigned char>(b[e]))) return false;
        return true;
    }

    // The next hit of scan string `i` at or after `pos`.
    bool Find(size_t i, size_t pos, const char* hay, const char* b, size_t n, Next& out) const {
        const size_t len = q_.scan[i].size();
        const bool word = !q_.regex && q_.wholeWord;
        while (pos < n) {
            const char* it = std::search(hay + pos, hay + n, searchers_[i]);
            if (it == hay + n) return false;
            const size_t s = static_cast<size_t>(it - hay);
            if (!word || WordOk(b, n, s, s + len)) {
                out.s = s;
                out.e = s + len;
                return true;
            }
            pos = s + 1;
        }
        return false;
    }

    bool NextMatch(size_t pos, const char* hay, const char* b, size_t n, size_t& s, size_t& e) {
        bool any = false;
        for (size_t i = 0; i < q_.scan.size(); ++i) {
            Next& nx = next_[i];
            if (nx.dead) continue;
            if (!nx.has || nx.s < pos) {
                nx.has = Find(i, pos, hay, b, n, nx);
                if (!nx.has) {
                    nx.dead = true;
                    continue;
                }
            }
            if (!any || nx.s < s || (nx.s == s && nx.e > e)) {
                s = nx.s;
                e = nx.e;
                any = true;
            }
        }
        return any;
    }

    void AddSpan(size_t s, size_t e) {
        if (spans_.size() >= kMaxRanges) return;
        if (!spans_.empty() && s <= spans_.back().second) {
            spans_.back().second = std::max(spans_.back().second, e);
            return;
        }
        spans_.emplace_back(s, e);
    }

    // Regex on one line (no CR/LF); fills spans_.
    void RegexLine(const char* l, size_t len) {
        spans_.clear();
        bool sawEmpty = false;
        std::vector<Span> all;
        for (const RegexForm& f : q_.forms) {
            size_t off = 0;
            while (off <= len) {
                const int rc = pcre2_match(f.code, reinterpret_cast<PCRE2_SPTR>(l), len, off, 0, md_, mc_);
                if (rc < 0) break;  // no match, or a limit was hit: no match on this line
                const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md_);
                const size_t s = ov[0], e = ov[1];
                if (s > e || e > len) break;
                if (s == e) {
                    if (!sawEmpty && all.empty()) all.emplace_back(s, s);
                    sawEmpty = true;
                    off = e + 1;
                    continue;
                }
                all.emplace_back(s, e);
                off = e;
                if (all.size() >= kMaxRanges) break;
            }
        }
        std::sort(all.begin(), all.end());
        for (const Span& sp : all) {
            if (sp.first == sp.second && !spans_.empty()) continue;
            AddSpan(sp.first, sp.second);
        }
    }

    bool Emit(const char* b, size_t ls, size_t le, uint32_t line) {
        size_t len = le - ls;
        if (len > 0 && b[ls + len - 1] == '\r') --len;
        for (Span& sp : spans_) {
            sp.first = std::min(sp.first, len);
            sp.second = std::min(sp.second, len);
        }
        if (!sink_.AddHit(*ctx_, line, b + ls, len, spans_)) {
            stopped_ = true;
            return false;
        }
        return true;
    }

    void ProcessBuffer(const char* b, size_t n, uint32_t firstLine) {
        if (n == 0 || sink_.Stopped()) return;
        stopped_ = false;
        if (!q_.useLiterals) {
            AllLines(b, n, firstLine);
            return;
        }
        const char* hay = b;
        if (q_.foldBuffer) {
            if (foldCap_ < n) {
                fold_.reset(new char[n]);
                foldCap_ = n;
            }
            FoldAscii(b, fold_.get(), n);
            hay = fold_.get();
        }
        for (Next& nx : next_) nx = Next();
        size_t pos = 0, counted = 0;
        uint32_t lineAt = firstLine;
        size_t s = 0, e = 0;
        while (pos < n && !sink_.Stopped() && NextMatch(pos, hay, b, n, s, e)) {
            size_t ls = s;
            while (ls > 0 && b[ls - 1] != '\n') --ls;
            const char* nl = static_cast<const char*>(std::memchr(b + s, '\n', n - s));
            const size_t le = nl ? static_cast<size_t>(nl - b) : n;
            if (ls > counted) {
                lineAt += static_cast<uint32_t>(CountNewlines(b + counted, ls - counted));
                counted = ls;
            }
            if (q_.regex) {
                size_t len = le - ls;
                if (len > 0 && b[ls + len - 1] == '\r') --len;
                RegexLine(b + ls, len);
                if (!spans_.empty()) {
                    // spans_ are relative to the line already
                    if (!EmitSpans(b, ls, le, lineAt)) return;
                }
            } else {
                spans_.clear();
                AddSpan(s - ls, e - ls);
                size_t p = e;
                size_t s2, e2;
                while (NextMatch(p, hay, b, n, s2, e2) && s2 < le) {
                    AddSpan(s2 - ls, e2 - ls);
                    p = e2;
                }
                if (!EmitSpans(b, ls, le, lineAt)) return;
            }
            pos = le + 1;
        }
    }

    bool EmitSpans(const char* b, size_t ls, size_t le, uint32_t line) { return Emit(b, ls, le, line); }

    void AllLines(const char* b, size_t n, uint32_t firstLine) {
        size_t pos = 0;
        uint32_t line = firstLine;
        while (pos < n && !sink_.Stopped()) {
            const char* nl = static_cast<const char*>(std::memchr(b + pos, '\n', n - pos));
            const size_t le = nl ? static_cast<size_t>(nl - b) : n;
            size_t len = le - pos;
            if (len > 0 && b[pos + len - 1] == '\r') --len;
            RegexLine(b + pos, len);
            if (!spans_.empty() && !Emit(b, pos, le, line)) return;
            pos = le + 1;
            ++line;
        }
    }

    const Query& q_;
    HitSink& sink_;
    FileCtx* ctx_ = nullptr;
    std::vector<std::boyer_moore_horspool_searcher<const char*>> searchers_;
    Next next_[2];
    std::vector<Span> spans_;
    std::vector<char> carry_;
    std::unique_ptr<char[]> fold_;
    size_t foldCap_ = 0;
    uint32_t lineNo_ = 1;
    bool skipping_ = false;
    bool stopped_ = false;
    pcre2_match_data* md_ = nullptr;
    pcre2_match_context* mc_ = nullptr;
};

// The displayed form of a matching line: decoded, cut around the first hit.
void BuildHit(const char* l, size_t len, const std::vector<Span>& spans, TextSearchHit& h) {
    const bool utf8 = ValidUtf8(reinterpret_cast<const unsigned char*>(l), len);
    size_t ws = 0, we = len;
    if (len > kShowChars) {
        const size_t first = spans.empty() ? 0 : std::min(spans[0].first, len);
        ws = first > 200 ? first - 200 : 0;
        we = std::min(len, ws + kShowChars);
        if (utf8) {
            while (ws < we && (static_cast<unsigned char>(l[ws]) & 0xC0) == 0x80) ++ws;
            while (we < len && we > ws && (static_cast<unsigned char>(l[we]) & 0xC0) == 0x80) --we;
        }
    }
    const UINT cp = utf8 ? CP_UTF8 : 1252;
    const int wn = we > ws ? MultiByteToWideChar(cp, 0, l + ws, static_cast<int>(we - ws), nullptr, 0) : 0;
    std::wstring t;
    if (ws > 0) t.push_back(L'\u2026');
    const size_t base = t.size();
    t.resize(base + static_cast<size_t>(std::max(wn, 0)));
    if (wn > 0) MultiByteToWideChar(cp, 0, l + ws, static_cast<int>(we - ws), t.data() + base, wn);
    if (we < len) t.push_back(L'\u2026');
    size_t curB = 0, curW = 0;
    auto toWide = [&](size_t byteOffset) -> uint32_t {
        const size_t rel = byteOffset - ws;
        while (curB < rel) {
            const unsigned char c = static_cast<unsigned char>(l[ws + curB]);
            if (!utf8) ++curW;
            else if ((c & 0xC0) != 0x80) curW += c >= 0xF0 ? 2 : 1;
            ++curB;
        }
        return static_cast<uint32_t>(base + curW);
    };
    for (const Span& sp : spans) {
        if (sp.second < ws || sp.first > we) continue;
        const size_t a = std::max(sp.first, ws), b = std::min(sp.second, we);
        if (b < a) continue;
        const uint32_t wa = toWide(a);
        const uint32_t wb = toWide(b);
        h.ranges.emplace_back(wa, wb);
    }
    h.text = std::move(t);
}

// ---------------------------------------------------------------- files

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out, size_t maxBytes, std::wstring& error) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = WindowsError(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        error = WindowsError(GetLastError());
        CloseHandle(h);
        return false;
    }
    if (static_cast<uint64_t>(size.QuadPart) > maxBytes) {
        error = L"The file is too large.";
        CloseHandle(h);
        return false;
    }
    try {
        out.resize(static_cast<size_t>(size.QuadPart));
    } catch (...) {
        error = L"Out of memory.";
        CloseHandle(h);
        return false;
    }
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 16u << 20));
        if (!ReadFile(h, out.data() + done, want, &got, nullptr)) {
            error = WindowsError(GetLastError());
            break;
        }
        if (got == 0) {  // shrank while reading
            out.resize(done);
            break;
        }
        done += got;
    }
    CloseHandle(h);
    return error.empty();
}

void Utf16ToUtf8(const uint8_t* d, size_t bytes, bool bigEndian, std::string& out) {
    const size_t units = bytes / 2;
    std::wstring w(units, L'\0');
    if (bigEndian) {
        for (size_t i = 0; i < units; ++i) w[i] = static_cast<wchar_t>((d[2 * i] << 8) | d[2 * i + 1]);
    } else {
        std::memcpy(w.data(), d, units * 2);
    }
    out = ToUtf8(w);
}

// Reads an archive entry by its stored path: the hash lookup first, then a walk
// comparing names (hashes can collide, and old tools wrote odd names).
bool ReadArchiveEntry(const std::wstring& archive, const std::string& inner, std::vector<uint8_t>& out, std::wstring& error) {
    bsa::Reader r;
    bsa::Status s = r.open(fs::path(LongPath(archive)));
    if (s != bsa::Status::Ok) {
        error = Widen(bsa::to_string(s));
        return false;
    }
    bsa::Entry e;
    s = r.find(inner, e);
    if (s != bsa::Status::Ok) {
        const std::string want = LowerAsciiCopy(inner);
        bool found = false;
        for (uint32_t i = 0; i < r.header().fileCount && !found; ++i) {
            std::string_view folder, name;
            if (r.entry(i, e) != bsa::Status::Ok || r.file_name(e, name) != bsa::Status::Ok) continue;
            r.folder_name(e.folderIndex, folder);
            std::string full = folder.empty() ? std::string(name) : std::string(folder) + "\\" + std::string(name);
            std::replace(full.begin(), full.end(), '/', '\\');
            std::string w = want;
            std::replace(w.begin(), w.end(), '/', '\\');
            if (LowerAsciiCopy(full) == w) found = true;
        }
        if (!found) {
            error = L"Not found in the archive.";
            return false;
        }
    }
    s = r.read(e, out, kMaxEntry);
    if (s != bsa::Status::Ok) {
        error = Widen(bsa::to_string(s));
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- the engine

struct TextSearch::Impl final : HitSink {
    enum class Kind { Folder, Loose, Archive, LoosePex, ArchivePex };
    struct Item {
        Kind kind = Kind::Folder;
        std::wstring path;
        std::wstring display;
        std::shared_ptr<std::vector<uint32_t>> entries;  // ArchivePex
    };
    struct PexEntry {
        uint32_t index;
        std::wstring stem;
    };
    struct PexArchive {
        std::wstring display;
        std::vector<PexEntry> entries;
    };
    struct PexLoose {
        std::wstring path, display, stem;
    };
    struct Worker {
        explicit Worker(Impl& impl) : scanner(impl.query, impl) {}
        Scanner scanner;
        FileCtx ctx;
        std::unique_ptr<char[]> readBuf;
        size_t readCap = 0;
        std::vector<uint8_t> entryBuf;
        WorkSlot* slot = nullptr;
    };

    TextSearchOptions opts;
    Query query;
    std::wstring root;
    bool archiveRoot = false;
    std::unordered_set<std::wstring> wantW;
    std::unordered_set<std::string> wantA;

    std::atomic<bool> stop{false}, cancelled{false}, truncated{false}, done{true};
    std::atomic<uint64_t> filesSearched{0}, bytesSearched{0}, archivesOpened{0}, foldersListed{0}, pexDecompiled{0}, skippedBinary{0}, errors{0};

    std::mutex mu;  // queue
    std::condition_variable cv;
    std::deque<Item> queue;
    size_t outstanding = 0;
    int phase = 1;

    mutable std::mutex dataMu;  // files, hits, first error
    std::vector<TextSearchFile> files;
    std::vector<TextSearchHit> pending;
    uint64_t hitTotal = 0;
    std::wstring firstError;

    std::mutex pexMu;
    std::unordered_set<std::wstring> pscStems;
    std::vector<PexLoose> pexLoose;
    std::map<std::wstring, PexArchive> pexArchives;  // by archive path

    std::vector<HANDLE> threads;
    std::atomic<int> running{0};
    std::mutex joinMu;
    std::chrono::steady_clock::time_point startTime{};
    std::atomic<int64_t> endNanos{-1};

    ~Impl() override {
        Stop(false);
        Join();
    }

    // -- HitSink
    bool Stopped() const override { return stop.load(std::memory_order_relaxed); }

    bool AddHit(FileCtx& f, uint32_t line, const char* text, size_t length, const std::vector<Span>& spans) override {
        if (Stopped()) return false;
        TextSearchHit h;
        h.line = line;
        BuildHit(text, length, spans, h);
        bool reachedLimit = false;
        {
            std::lock_guard<std::mutex> lock(dataMu);
            if (hitTotal >= opts.maxHits) {
                truncated = true;
                reachedLimit = true;
            } else {
                if (f.index < 0) {
                    TextSearchFile tf;
                    tf.display = f.display;
                    tf.diskPath = f.diskPath;
                    tf.inner = f.inner;
                    tf.decompiled = f.decompiled;
                    files.push_back(std::move(tf));
                    f.index = static_cast<int64_t>(files.size()) - 1;
                }
                h.file = static_cast<uint32_t>(f.index);
                pending.push_back(std::move(h));
                ++hitTotal;
            }
        }
        if (reachedLimit) {
            Stop(false);
            return false;
        }
        return true;
    }

    void Stop(bool byUser) {
        if (byUser && !done) cancelled = true;
        stop = true;
        { std::lock_guard<std::mutex> lock(mu); }
        cv.notify_all();
    }

    void AddError(const std::wstring& what) {
        ++errors;
        std::lock_guard<std::mutex> lock(dataMu);
        if (firstError.empty()) firstError = what;
    }

    void Join() {
        std::lock_guard<std::mutex> lock(joinMu);
        for (HANDLE h : threads) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
        threads.clear();
    }

    // -- queue
    void Push(std::vector<Item>& items) {
        if (items.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            outstanding += items.size();
            for (Item& it : items) queue.push_back(std::move(it));
        }
        items.clear();
        cv.notify_all();
    }

    // One item is done. When phase 1 drains, the .pex without a .psc become phase 2.
    void Finish() {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (--outstanding == 0) {
                if (phase == 1 && opts.decompilePex && !Stopped()) {
                    phase = 2;
                    QueuePhase2();
                }
                wake = true;
            }
        }
        if (wake) cv.notify_all();
    }

    void QueuePhase2() {  // called with `mu` held
        std::lock_guard<std::mutex> lock(pexMu);
        for (PexLoose& p : pexLoose) {
            if (pscStems.count(p.stem)) continue;
            Item it;
            it.kind = Kind::LoosePex;
            it.path = p.path;
            it.display = p.display;
            queue.push_back(std::move(it));
            ++outstanding;
        }
        for (auto& [path, group] : pexArchives) {
            std::shared_ptr<std::vector<uint32_t>> batch;
            for (const PexEntry& e : group.entries) {
                if (pscStems.count(e.stem)) continue;
                if (!batch) batch = std::make_shared<std::vector<uint32_t>>();
                batch->push_back(e.index);
                if (batch->size() >= kPexBatch) {
                    PushArchivePex(path, group.display, batch);
                    batch.reset();
                }
            }
            if (batch) PushArchivePex(path, group.display, batch);
        }
    }

    void PushArchivePex(const std::wstring& path, const std::wstring& display, const std::shared_ptr<std::vector<uint32_t>>& batch) {
        Item it;
        it.kind = Kind::ArchivePex;
        it.path = path;
        it.display = display;
        it.entries = batch;
        queue.push_back(std::move(it));
        ++outstanding;
    }

    // -- workers
    static DWORD WINAPI ThreadMain(LPVOID param) {
        static_cast<Impl*>(param)->RunWorker();
        return 0;
    }

    void RunWorker() {
        try {
            Worker w(*this);
            std::unique_ptr<WorkSlot> slot;
            if (opts.threads <= 0) {
                slot = std::make_unique<WorkSlot>(WorkKind::Interactive, &stop);
                w.slot = slot.get();
            }
            if (!slot || slot->Held()) {
                for (;;) {
                    Item it;
                    {
                        std::unique_lock<std::mutex> lock(mu);
                        cv.wait(lock, [&] { return Stopped() || !queue.empty() || outstanding == 0; });
                        if (Stopped() || queue.empty()) break;
                        it = std::move(queue.front());
                        queue.pop_front();
                    }
                    try {
                        Process(it, w);
                    } catch (const std::bad_alloc&) {
                        AddError(it.display + L": out of memory");
                    } catch (...) {
                        AddError(it.display + L": unexpected error");
                    }
                    w.scanner.Trim();
                    Finish();
                    if (slot) slot->Pace();
                }
            }
        } catch (...) {
            AddError(L"A search thread couldn't start (out of memory).");
        }
        if (--running == 0) {
            endNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startTime).count();
            done = true;
        }
    }

    void Process(Item& it, Worker& w) {
        switch (it.kind) {
            case Kind::Folder: ListFolder(it); break;
            case Kind::Loose: SearchLoose(it, w); break;
            case Kind::Archive: SearchArchive(it, w); break;
            case Kind::LoosePex: SearchLoosePex(it, w); break;
            case Kind::ArchivePex: SearchArchivePex(it, w); break;
        }
    }

    void Pace(Worker& w) {
        if (w.slot) w.slot->Pace();
    }

    bool WantedLoose(const std::wstring& ext) const { return wantW.count(ext) != 0; }

    void ListFolder(const Item& folder) {
        ++foldersListed;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(LongPath(JoinPath(folder.path, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) AddError(folder.display.empty() ? folder.path : folder.display + L": " + WindowsError(code));
            return;
        }
        std::vector<Item> batch;
        std::vector<std::wstring> stems;
        std::vector<PexLoose> pex;
        do {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // junctions and symlinks
                if (!opts.otherText && IsSkippedFolder(name)) continue;
                Item it;
                it.kind = Kind::Folder;
                it.path = JoinPath(folder.path, name);
                it.display = folder.display.empty() ? std::wstring(name) : folder.display + L"\\" + name;
                batch.push_back(std::move(it));
                continue;
            }
            const std::wstring file = name;
            const std::wstring ext = ExtensionOf(file);
            if (ext.empty()) continue;
            Item it;
            it.path = JoinPath(folder.path, file);
            it.display = folder.display.empty() ? file : folder.display + L"\\" + file;
            if (ext == L".bsa") {
                it.kind = Kind::Archive;
            } else if (ext == L".psc") {
                it.kind = Kind::Loose;
                if (opts.decompilePex) stems.push_back(Lower(file.substr(0, file.size() - ext.size())));
            } else if (ext == L".pex" && opts.decompilePex) {
                PexLoose p;
                p.stem = Lower(file.substr(0, file.size() - ext.size()));
                p.path = it.path;
                p.display = it.display;
                pex.push_back(std::move(p));
                continue;
            } else if (opts.otherText && WantedLoose(ext)) {
                it.kind = Kind::Loose;
            } else {
                continue;
            }
            batch.push_back(std::move(it));
        } while (!Stopped() && FindNextFileW(h, &fd));
        FindClose(h);
        if (!stems.empty() || !pex.empty()) {
            std::lock_guard<std::mutex> lock(pexMu);
            for (std::wstring& s : stems) pscStems.insert(std::move(s));
            for (PexLoose& p : pex) pexLoose.push_back(std::move(p));
        }
        Push(batch);
    }

    // The bytes of one file or entry, from memory.
    void SearchMemory(Worker& w, FileCtx& ctx, const uint8_t* d, size_t n, bool sniff) {
        if (n == 0) {
            ++filesSearched;
            return;
        }
        if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
            if (n > kMaxUtf16 + 2) {
                AddError(ctx.display + L": too large to search as UTF-16");
                return;
            }
            std::string utf8;
            Utf16ToUtf8(d + 2, n - 2, d[0] == 0xFE, utf8);
            ++filesSearched;
            bytesSearched += n;
            w.scanner.Begin(&ctx);
            w.scanner.Feed(utf8.data(), utf8.size());
            w.scanner.End();
            return;
        }
        size_t off = 0;
        if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) off = 3;
        if (sniff && std::memchr(d + off, 0, std::min(n - off, kSniff))) {
            ++skippedBinary;
            return;
        }
        ++filesSearched;
        bytesSearched += n;
        w.scanner.Begin(&ctx);
        w.scanner.Feed(reinterpret_cast<const char*>(d) + off, n - off);
        w.scanner.End();
    }

    void SearchLoose(const Item& it, Worker& w) {
        HANDLE h = CreateFileW(LongPath(it.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            AddError(it.display + L": " + WindowsError(GetLastError()));
            return;
        }
        struct Closer {
            HANDLE h;
            ~Closer() { CloseHandle(h); }
        } closer{h};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size)) size.QuadPart = static_cast<LONGLONG>(kChunk);
        const size_t want = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(size.QuadPart) + 1, kChunk));
        if (w.readCap < want) {
            w.readBuf.reset(new char[want]);
            w.readCap = want;
        }
        DWORD got = 0;
        if (!ReadFile(h, w.readBuf.get(), static_cast<DWORD>(std::min(w.readCap, kChunk)), &got, nullptr)) {
            AddError(it.display + L": " + WindowsError(GetLastError()));
            return;
        }
        w.ctx = FileCtx();
        w.ctx.display = it.display;
        w.ctx.diskPath = it.path;
        if (got == 0) {
            ++filesSearched;
            return;
        }
        const uint8_t* d = reinterpret_cast<const uint8_t*>(w.readBuf.get());
        if (got >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
            // UTF-16: converted whole.
            std::vector<uint8_t> all(d, d + got);
            for (;;) {
                if (Stopped()) return;
                if (all.size() > kMaxUtf16 + 2) {
                    AddError(it.display + L": too large to search as UTF-16");
                    return;
                }
                const size_t old = all.size();
                all.resize(old + (1u << 20));
                DWORD more = 0;
                if (!ReadFile(h, all.data() + old, 1u << 20, &more, nullptr)) {
                    AddError(it.display + L": " + WindowsError(GetLastError()));
                    return;
                }
                all.resize(old + more);
                if (more == 0) break;
            }
            SearchMemory(w, w.ctx, all.data(), all.size(), false);
            return;
        }
        size_t off = 0;
        if (got >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) off = 3;
        if (std::memchr(d + off, 0, std::min<size_t>(got - off, kSniff))) {
            ++skippedBinary;
            return;
        }
        ++filesSearched;
        uint64_t total = got;
        w.scanner.Begin(&w.ctx);
        w.scanner.Feed(w.readBuf.get() + off, got - off);
        while (!Stopped()) {
            DWORD more = 0;
            if (!ReadFile(h, w.readBuf.get(), static_cast<DWORD>(std::min(w.readCap, kChunk)), &more, nullptr)) {
                AddError(it.display + L": " + WindowsError(GetLastError()));
                break;
            }
            if (more == 0) break;
            total += more;
            w.scanner.Feed(w.readBuf.get(), more);
        }
        w.scanner.End();
        bytesSearched += total;
    }

    void SearchLoosePex(const Item& it, Worker& w) {
        std::wstring error;
        if (!ReadWholeFile(it.path, w.entryBuf, kMaxEntry, error)) {
            AddError(it.display + L": " + error);
            return;
        }
        SearchDecompiled(w, it.path, std::string(), it.display, w.entryBuf);
    }

    void SearchDecompiled(Worker& w, const std::wstring& diskPath, const std::string& inner, const std::wstring& display, const std::vector<uint8_t>& bytes) {
        std::string source;
        std::wstring error;
        if (!DecompilePexBytes(bytes.data(), bytes.size(), source, error)) {
            AddError(display + L" " + error);
            return;
        }
        ++pexDecompiled;
        w.ctx = FileCtx();
        w.ctx.display = display + L" (decompiled)";
        w.ctx.diskPath = diskPath;
        w.ctx.inner = inner;
        w.ctx.decompiled = true;
        SearchMemory(w, w.ctx, reinterpret_cast<const uint8_t*>(source.data()), source.size(), false);
    }

    static bool EntryName(const bsa::Reader& r, const bsa::Entry& e, std::string& inner, std::string_view& name) {
        std::string_view folder;
        if (r.file_name(e, name) != bsa::Status::Ok) return false;
        if (r.folder_name(e.folderIndex, folder) != bsa::Status::Ok) folder = {};
        inner = folder.empty() ? std::string(name) : std::string(folder) + "\\" + std::string(name);
        return true;
    }

    void SearchArchive(const Item& it, Worker& w) {
        bsa::Reader r;
        const bsa::Status s = r.open(fs::path(LongPath(it.path)));
        if (s != bsa::Status::Ok) {
            AddError(it.display + L": " + Widen(bsa::to_string(s)));
            return;
        }
        ++archivesOpened;
        const uint32_t count = r.header().fileCount;
        for (uint32_t i = 0; i < count && !Stopped(); ++i) {
            bsa::Entry e;
            if (r.entry(i, e) != bsa::Status::Ok) continue;
            std::string_view name;
            if (r.file_name(e, name) != bsa::Status::Ok) {
                AddError(it.display + L": the archive has no usable file names");
                return;
            }
            const std::string ext = ExtensionOfNarrow(name);
            if (ext.empty()) continue;
            const bool psc = ext == ".psc";
            const bool pex = opts.decompilePex && ext == ".pex";
            if (!psc && !pex && !(opts.otherText && wantA.count(ext))) continue;
            std::string inner;
            EntryName(r, e, inner, name);
            const std::wstring innerW = BackslashPath(Widen(inner));
            if (psc || pex) {
                if (opts.decompilePex) {
                    std::wstring stem = Lower(BackslashPath(Widen(name)));
                    stem.resize(stem.size() - ext.size());
                    std::lock_guard<std::mutex> lock(pexMu);
                    if (psc) {
                        pscStems.insert(std::move(stem));
                    } else {
                        PexArchive& g = pexArchives[it.path];
                        if (g.display.empty()) g.display = it.display;
                        g.entries.push_back({i, std::move(stem)});
                    }
                }
                if (pex) continue;
            }
            const bsa::Status rs = r.read(e, w.entryBuf, kMaxEntry);
            if (rs != bsa::Status::Ok) {
                AddError(it.display + L"\\" + innerW + L": " + Widen(bsa::to_string(rs)));
                continue;
            }
            w.ctx = FileCtx();
            w.ctx.display = it.display + L"\\" + innerW;
            w.ctx.diskPath = it.path;
            w.ctx.inner = inner;
            SearchMemory(w, w.ctx, w.entryBuf.data(), w.entryBuf.size(), true);
            Pace(w);
        }
    }

    void SearchArchivePex(const Item& it, Worker& w) {
        bsa::Reader r;
        const bsa::Status s = r.open(fs::path(LongPath(it.path)));
        if (s != bsa::Status::Ok) {
            AddError(it.display + L": " + Widen(bsa::to_string(s)));
            return;
        }
        ++archivesOpened;
        for (uint32_t index : *it.entries) {
            if (Stopped()) break;
            bsa::Entry e;
            std::string inner;
            std::string_view name;
            if (r.entry(index, e) != bsa::Status::Ok || !EntryName(r, e, inner, name)) continue;
            const std::wstring display = it.display + L"\\" + BackslashPath(Widen(inner));
            const bsa::Status rs = r.read(e, w.entryBuf, kMaxEntry);
            if (rs != bsa::Status::Ok) {
                AddError(display + L": " + Widen(bsa::to_string(rs)));
                continue;
            }
            SearchDecompiled(w, it.path, inner, display, w.entryBuf);
            Pace(w);
        }
    }
};

// ---------------------------------------------------------------- TextSearch

TextSearch::TextSearch() : impl_(std::make_unique<Impl>()) {}

TextSearch::~TextSearch() {
    if (impl_) {
        impl_->Stop(true);
        impl_->Join();
    }
}

bool TextSearch::Start(const std::wstring& rootIn, const TextSearchOptions& options) {
    Cancel();
    Wait();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.opts = options;
    m.startTime = std::chrono::steady_clock::now();
    auto fail = [&](const std::wstring& why) {
        m.firstError = why;
        m.done = true;
        m.endNanos = 0;
        return false;
    };
    if (options.query.empty()) return fail(L"Nothing to search for.");
    std::wstring error;
    if (!BuildQuery(options, m.query, error)) return fail(error);

    // The scope: an absolute path, backslashes, no trailing slash.
    std::wstring r = rootIn;
    std::replace(r.begin(), r.end(), L'/', L'\\');
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(r.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (n > 0 && n < std::size(full)) r.assign(full, n);
    while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    m.root = r;
    const DWORD attrs = GetFileAttributesW(LongPath(r).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return fail(L"Not found: " + r);
    Impl::Item first;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        first.kind = Impl::Kind::Folder;
        first.path = r;
    } else if (ExtensionOf(r) == L".bsa") {
        m.archiveRoot = true;
        first.kind = Impl::Kind::Archive;
        first.path = r;
        first.display = LeafOf(r);
    } else {
        return fail(L"Not a folder or a .bsa archive: " + r);
    }

    for (const std::wstring& e : options.extraExtensions) {
        std::wstring ext = Lower(e);
        if (ext.empty() || ext == L".bsa" || ext == L".psc" || ext == L".pex") continue;
        if (ext[0] != L'.') ext.insert(ext.begin(), L'.');
        m.wantW.insert(ext);
        m.wantA.insert(LowerAsciiCopy(ToUtf8(ext)));
    }

    int count = options.threads > 0 ? std::clamp(options.threads, 1, 16) : std::clamp(WorkerLimit(), 1, 16);
    m.done = false;
    m.outstanding = 1;
    m.queue.push_back(std::move(first));
    m.threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        HANDLE h = CreateThread(nullptr, 8u << 20, &Impl::ThreadMain, &m, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h) m.threads.push_back(h);
    }
    if (m.threads.empty()) {
        m.done = true;
        return fail(L"Couldn't start the search threads.");
    }
    m.running = static_cast<int>(m.threads.size());
    m.startTime = std::chrono::steady_clock::now();
    for (HANDLE h : m.threads) ResumeThread(h);
    return true;
}

void TextSearch::Cancel() {
    if (impl_) impl_->Stop(true);
}

void TextSearch::Wait() {
    if (impl_) impl_->Join();
}

size_t TextSearch::TakeHits(std::vector<TextSearchHit>& out) {
    std::lock_guard<std::mutex> lock(impl_->dataMu);
    const size_t n = impl_->pending.size();
    out.reserve(out.size() + n);
    for (TextSearchHit& h : impl_->pending) out.push_back(std::move(h));
    impl_->pending.clear();
    return n;
}

std::vector<TextSearchFile> TextSearch::Files() const {
    std::lock_guard<std::mutex> lock(impl_->dataMu);
    return impl_->files;
}

TextSearchFile TextSearch::File(uint32_t index) const {
    std::lock_guard<std::mutex> lock(impl_->dataMu);
    return index < impl_->files.size() ? impl_->files[index] : TextSearchFile();
}

TextSearchStats TextSearch::Stats() const {
    const Impl& m = *impl_;
    TextSearchStats s;
    s.filesSearched = m.filesSearched;
    s.bytesSearched = m.bytesSearched;
    s.archivesOpened = m.archivesOpened;
    s.foldersListed = m.foldersListed;
    s.pexDecompiled = m.pexDecompiled;
    s.skippedBinary = m.skippedBinary;
    s.errors = m.errors;
    s.done = m.done;
    s.cancelled = m.cancelled;
    s.truncated = m.truncated;
    {
        std::lock_guard<std::mutex> lock(m.dataMu);
        s.hits = m.hitTotal;
        s.filesWithHits = m.files.size();
        s.firstError = m.firstError;
    }
    const int64_t end = m.endNanos;
    s.seconds = end >= 0 ? static_cast<double>(end) / 1e9
                         : std::chrono::duration<double>(std::chrono::steady_clock::now() - m.startTime).count();
    return s;
}

const std::wstring& TextSearch::Root() const { return impl_->root; }

// ---------------------------------------------------------------- opening a result

bool ReadSearchFileBytes(const TextSearchFile& file, std::string& bytes, std::wstring& error) {
    std::vector<uint8_t> raw;
    try {
        if (file.inner.empty()) {
            if (!ReadWholeFile(file.diskPath, raw, size_t{1} << 30, error)) return false;
        } else if (!ReadArchiveEntry(file.diskPath, file.inner, raw, error)) {
            return false;
        }
        if (file.decompiled) {
            std::wstring why;
            if (!DecompilePexBytes(raw.data(), raw.size(), bytes, why)) {
                error = L"The script " + why;
                return false;
            }
            return true;
        }
        bytes.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    } catch (const std::bad_alloc&) {
        error = L"Out of memory.";
        return false;
    }
    return true;
}

namespace {

std::wstring SearchCopyRoot() {
    wchar_t temp[MAX_PATH * 2] = {};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (n == 0 || n >= std::size(temp)) return {};
    std::wstring t(temp, n);
    while (!t.empty() && t.back() == L'\\') t.pop_back();
    return t + L"\\WHAM search";
}

std::vector<std::wstring> SplitPath(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

bool MakeFolders(const std::wstring& folder) {
    std::wstring so_far;
    for (const std::wstring& part : SplitPath(folder)) {
        so_far = so_far.empty() ? part : so_far + L"\\" + part;
        if (so_far.size() <= 3 && so_far.find(L':') != std::wstring::npos) continue;  // a drive
        if (so_far.empty()) continue;
        if (!CreateDirectoryW(LongPath(so_far).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

}  // namespace

bool WriteSearchCopy(const TextSearchFile& file, std::wstring& path, std::wstring& error) {
    if (file.inner.empty() && !file.decompiled) {
        path = file.diskPath;
        return true;
    }
    const std::wstring top = SearchCopyRoot();
    if (top.empty()) {
        error = L"Can't find the temp folder.";
        return false;
    }
    // Names from the archive are untrusted: every piece must be a plain file name.
    std::wstring archiveName, folder = top;
    std::wstring leaf;
    if (!file.inner.empty()) {
        archiveName = LeafOf(file.diskPath);
        std::vector<std::wstring> parts = SplitPath(Widen(file.inner));
        if (!SafeName(archiveName)) {
            error = L"The archive name isn't a safe file name.";
            return false;
        }
        folder += L"\\" + archiveName;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (!SafeName(parts[i])) {
                error = L"The path inside the archive isn't safe to write.";
                return false;
            }
            folder += L"\\" + parts[i];
        }
        leaf = parts.back();
    } else {
        folder += L"\\decompiled";
        leaf = LeafOf(file.diskPath);
    }
    if (!SafeName(leaf)) {
        error = L"The file name isn't safe to write.";
        return false;
    }
    const size_t dot = leaf.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? leaf : leaf.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : leaf.substr(dot);
    std::wstring tag;
    if (!archiveName.empty()) tag = archiveName + (file.decompiled ? L", decompiled" : L"");
    else tag = L"decompiled";
    if (file.decompiled) ext = L".psc";
    const std::wstring name = stem + L" (" + tag + L")" + ext;
    if (!SafeName(name)) {
        error = L"The file name isn't safe to write.";
        return false;
    }
    const std::wstring target = folder + L"\\" + name;

    std::string bytes;
    if (!ReadSearchFileBytes(file, bytes, error)) return false;
    if (!MakeFolders(folder)) {
        error = L"Can't create " + folder + L": " + WindowsError(GetLastError());
        return false;
    }
    {
        std::vector<uint8_t> old;
        std::wstring ignore;
        if (ReadWholeFile(target, old, bytes.size() + 1, ignore) && old.size() == bytes.size() &&
            (bytes.empty() || std::memcmp(old.data(), bytes.data(), bytes.size()) == 0)) {
            path = target;
            return true;
        }
    }
    const std::wstring lp = LongPath(target);
    const DWORD attrs = GetFileAttributesW(lp.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(lp.c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Can't write " + target + L": " + WindowsError(GetLastError());
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (done < bytes.size() && ok) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 16u << 20));
        ok = WriteFile(h, bytes.data() + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    const DWORD code = GetLastError();
    CloseHandle(h);
    if (!ok) {
        error = L"Can't write " + target + L": " + WindowsError(code);
        return false;
    }
    SetFileAttributesW(lp.c_str(), FILE_ATTRIBUTE_READONLY);
    path = target;
    return true;
}

namespace {

// Returns true when the folder ended up empty.
bool CleanFolder(const std::wstring& folder, uint64_t nowFileTime, bool& removedAny) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = folder + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            empty = false;  // never follow or touch links
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            bool inner = false;
            if (CleanFolder(full, nowFileTime, inner) && RemoveDirectoryW(LongPath(full).c_str())) continue;
            empty = false;
            continue;
        }
        const uint64_t written = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (nowFileTime > written && (nowFileTime - written) / 10000000ull >= kCopyMaxAgeSeconds) {
            const std::wstring lp = LongPath(full);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(lp.c_str(), fd.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
            if (DeleteFileW(lp.c_str())) {
                removedAny = true;
                continue;
            }
        }
        empty = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

}  // namespace

void CleanUpSearchCopies() {
    const std::wstring top = SearchCopyRoot();
    if (top.empty()) return;
    const DWORD attrs = GetFileAttributesW(LongPath(top).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    bool removed = false;
    CleanFolder(top, (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime, removed);
}

// ---------------------------------------------------------------- editor

namespace {

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::wstring s = buf;
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

std::wstring EnvString(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

bool Launch(const std::wstring& commandLine) {
    std::wstring cmd = commandLine;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

std::wstring FindNotepadPlusPlus() {
    const wchar_t* appPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\notepad++.exe";
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        const std::wstring p = RegString(root, appPaths, nullptr);
        if (!p.empty() && FileExists(p)) return p;
    }
    for (const wchar_t* key : {L"SOFTWARE\\Notepad++", L"SOFTWARE\\WOW6432Node\\Notepad++"}) {
        const std::wstring dir = RegString(HKEY_LOCAL_MACHINE, key, nullptr);
        if (dir.empty()) continue;
        const std::wstring p = JoinPath(dir, L"notepad++.exe");
        if (FileExists(p)) return p;
    }
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        const std::wstring dir = EnvString(var);
        if (dir.empty()) continue;
        const std::wstring p = JoinPath(dir, L"Notepad++\\notepad++.exe");
        if (FileExists(p)) return p;
    }
    return {};
}

bool OpenInEditor(const std::wstring& path, uint32_t line, std::wstring& error) {
    const std::wstring npp = FindNotepadPlusPlus();
    if (!npp.empty()) {
        std::wstring cmd = L"\"" + npp + L"\"";
        if (line > 0) cmd += L" -n" + std::to_wstring(line);
        cmd += L" \"" + path + L"\"";
        if (Launch(cmd)) return true;
    }
    // The mask is the user's: a .bat or .js added to it must be shown, never run.
    if (AssocIsDangerous(PathFindExtensionW(path.c_str()))) {
        if (Launch(L"notepad.exe \"" + path + L"\"")) return true;
        error = L"Couldn't open " + path + L": " + WindowsError(GetLastError());
        return false;
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    if (Launch(L"notepad.exe \"" + path + L"\"")) return true;
    error = L"Couldn't open " + path + L": " + WindowsError(GetLastError());
    return false;
}

}  // namespace pse
