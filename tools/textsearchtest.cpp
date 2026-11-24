// Tests for the Search text engine (src/text_search.*) and the wham-search
// command line tool. Everything is written under a fresh folder in %TEMP%
// (removed at the end unless --keep), and %TEMP% itself is pointed at that
// folder while the tests run, so the user's own "WHAM search" folder is never
// touched. The only things read from outside are a few small .pex files from
// the game's Data\Scripts (read-only copies).
//
//   textsearchtest <path to wham-search.exe> [--keep] [--no-big] [--no-scale]
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <bsa/reader.hpp>

#include "papyrus_tools.h"
#include "text_search.h"
#include "work_budget.h"

namespace fs = std::filesystem;
using namespace pse;

namespace {

int g_pass = 0, g_fail = 0;

void Check(const std::string& name, bool ok, const std::string& detail = std::string()) {
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), (ok || detail.empty()) ? "" : "  -- ", ok ? "" : detail.c_str());
    std::fflush(stdout);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += x + " | ";
    return s;
}

// ---------------------------------------------------------------- files

void WriteFileBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) {
        std::fprintf(stderr, "can't write %s\n", path.string().c_str());
        std::exit(3);
    }
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

std::string ReadFileBytes(const fs::path& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void ClearReadOnly(const fs::path& root) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const DWORD a = GetFileAttributesW(it->path().c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(it->path().c_str(), a & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
}

// ---------------------------------------------------------------- a minimal BSA writer

void Put32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void Put64(std::string& s, uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }

std::string ZlibStored(const std::string& data) {
    std::string out = "\x78\x01";
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, data.size() - pos);
        const bool last = pos + n >= data.size();
        out.push_back(last ? 1 : 0);
        const uint16_t len = static_cast<uint16_t>(n), nlen = static_cast<uint16_t>(~n);
        out.append(reinterpret_cast<const char*>(&len), 2);
        out.append(reinterpret_cast<const char*>(&nlen), 2);
        out.append(data, pos, n);
        pos += n;
    } while (pos < data.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((adler >> (8 * i)) & 0xFF));
    return out;
}

// An LZ4 block holding only literals (a valid block with no matches).
std::string Lz4LiteralBlock(const std::string& data) {
    std::string out;
    const size_t lit = data.size();
    if (lit < 15) {
        out.push_back(static_cast<char>(lit << 4));
    } else {
        out.push_back(static_cast<char>(15 << 4));
        size_t rest = lit - 15;
        while (rest >= 255) {
            out.push_back(static_cast<char>(255));
            rest -= 255;
        }
        out.push_back(static_cast<char>(rest));
    }
    out += data;
    return out;
}

std::string Lz4Frame(const std::string& data) {
    std::string out;
    Put32(out, 0x184D2204u);
    out.push_back(0x60);
    out.push_back(0x40);  // 64 KiB blocks
    out.push_back(0);     // header checksum (the reader doesn't check it)
    size_t pos = 0;
    int k = 0;
    while (pos < data.size()) {
        const size_t n = std::min<size_t>(65536, data.size() - pos);
        const std::string piece = data.substr(pos, n);
        if (k++ % 2 == 0) {  // stored block
            Put32(out, 0x80000000u | static_cast<uint32_t>(n));
            out += piece;
        } else {  // an encoded block (literals only)
            const std::string block = Lz4LiteralBlock(piece);
            Put32(out, static_cast<uint32_t>(block.size()));
            out += block;
        }
        pos += n;
    }
    Put32(out, 0);
    return out;
}

struct BsaFileSpec {
    std::string inner;  // "scripts\\source\\foo.psc"
    std::string data;
};

std::string BuildBsa(uint32_t version, bool compressed, const std::vector<BsaFileSpec>& specs) {
    struct F {
        std::string name, data;
        uint64_t hash;
    };
    struct D {
        std::string name;
        uint64_t hash;
        std::vector<F> files;
    };
    std::map<std::string, D> dirs;
    for (const BsaFileSpec& s : specs) {
        const size_t slash = s.inner.find_last_of('\\');
        const std::string folder = LowerAscii(s.inner.substr(0, slash));
        const std::string name = LowerAscii(s.inner.substr(slash + 1));
        D& d = dirs[folder];
        d.name = folder;
        d.hash = bsa::hash_folder(folder, version);
        d.files.push_back({name, s.data, bsa::hash_file(name, version)});
    }
    std::vector<D*> order;
    for (auto& [k, d] : dirs) {
        std::sort(d.files.begin(), d.files.end(), [](const F& a, const F& b) { return a.hash < b.hash; });
        order.push_back(&d);
    }
    std::sort(order.begin(), order.end(), [](const D* a, const D* b) { return a->hash < b->hash; });

    uint32_t folderNames = 0, fileNames = 0, fileCount = 0;
    for (const D* d : order) {
        folderNames += static_cast<uint32_t>(d->name.size() + 1);
        for (const F& f : d->files) fileNames += static_cast<uint32_t>(f.name.size() + 1), ++fileCount;
    }
    const size_t recSize = version == 105 ? 24 : 16;
    size_t pos = 36 + order.size() * recSize;
    std::vector<size_t> blockOffsets;
    for (const D* d : order) {
        blockOffsets.push_back(pos);
        pos += 1 + d->name.size() + 1 + 16 * d->files.size();
    }
    pos += fileNames;
    // payloads
    std::vector<std::string> payloads;
    for (const D* d : order)
        for (const F& f : d->files) {
            if (!compressed) {
                payloads.push_back(f.data);
            } else {
                std::string p;
                Put32(p, static_cast<uint32_t>(f.data.size()));
                p += version == 105 ? Lz4Frame(f.data) : ZlibStored(f.data);
                payloads.push_back(std::move(p));
            }
        }
    std::string out;
    out.append("BSA\0", 4);
    Put32(out, version);
    Put32(out, 36);
    Put32(out, 0x3 | (compressed ? 0x4 : 0));
    Put32(out, static_cast<uint32_t>(order.size()));
    Put32(out, fileCount);
    Put32(out, folderNames);
    Put32(out, fileNames);
    Put32(out, 0x100);
    for (size_t i = 0; i < order.size(); ++i) {
        Put64(out, order[i]->hash);
        Put32(out, static_cast<uint32_t>(order[i]->files.size()));
        const uint32_t offset = static_cast<uint32_t>(blockOffsets[i] + fileNames);
        if (version == 105) {
            Put32(out, 0);
            Put64(out, offset);
        } else {
            Put32(out, offset);
        }
    }
    size_t dataPos = pos, idx = 0;
    for (const D* d : order) {
        out.push_back(static_cast<char>(d->name.size() + 1));
        out += d->name;
        out.push_back(0);
        for (const F& f : d->files) {
            Put64(out, f.hash);
            Put32(out, static_cast<uint32_t>(payloads[idx].size()));
            Put32(out, static_cast<uint32_t>(dataPos));
            dataPos += payloads[idx].size();
            ++idx;
        }
    }
    for (const D* d : order)
        for (const F& f : d->files) {
            out += f.name;
            out.push_back(0);
        }
    for (const std::string& p : payloads) out += p;
    return out;
}

// ---------------------------------------------------------------- the model

enum class Enc { Utf8, Utf8Bom, Cp1252, Utf16 };

struct Doc {
    std::string display;  // as the engine shows it, UTF-8
    std::vector<std::string> lines;
    bool text = false;  // only searched with "other text types"
};

std::vector<Doc> g_docs;

std::string Encode(const std::vector<std::string>& lines, const std::string& eol, bool finalEol, Enc enc) {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        if (i + 1 < lines.size() || finalEol) s += eol;
    }
    if (enc == Enc::Utf8) return s;
    if (enc == Enc::Utf8Bom) return "\xEF\xBB\xBF" + s;
    const std::wstring w = Wide(s);
    if (enc == Enc::Utf16) {
        std::string out = "\xFF\xFE";
        out.append(reinterpret_cast<const char*>(w.data()), w.size() * 2);
        return out;
    }
    std::string out(w.size(), '\0');
    BOOL used = FALSE;
    WideCharToMultiByte(1252, 0, w.data(), static_cast<int>(w.size()), out.data(), static_cast<int>(out.size()), nullptr, &used);
    return out;
}

struct DocSpec {
    std::string rel;  // loose path, or the inner path in an archive
    std::vector<std::string> lines;
    std::string eol = "\n";
    bool finalEol = true;
    Enc enc = Enc::Utf8;
};

bool IsTextExt(const std::string& rel) { return !(rel.size() >= 4 && LowerAscii(rel.substr(rel.size() - 4)) == ".psc"); }

void AddLoose(const fs::path& root, const DocSpec& d) {
    WriteFileBytes(root / fs::path(Wide(d.rel)), Encode(d.lines, d.eol, d.finalEol, d.enc));
    g_docs.push_back({d.rel, d.lines, IsTextExt(d.rel)});
}

std::string AddBsa(const fs::path& root, const std::string& rel, uint32_t version, bool compressed, const std::vector<DocSpec>& docs,
                   const std::vector<BsaFileSpec>& extra = {}) {
    std::vector<BsaFileSpec> specs = extra;
    for (const DocSpec& d : docs) {
        specs.push_back({d.rel, Encode(d.lines, d.eol, d.finalEol, d.enc)});
        g_docs.push_back({rel + "\\" + d.rel, d.lines, IsTextExt(d.rel)});
    }
    const std::string bytes = BuildBsa(version, compressed, specs);
    WriteFileBytes(root / fs::path(Wide(rel)), bytes);
    return bytes;
}

using Pred = std::function<bool(const std::string&)>;

Pred Literal(std::string needle, bool matchCase, bool word) {
    return [=](const std::string& line) {
        const std::string h = matchCase ? line : LowerAscii(line);
        const std::string n = matchCase ? needle : LowerAscii(needle);
        size_t pos = 0;
        while ((pos = h.find(n, pos)) != std::string::npos) {
            const auto isWord = [&](size_t i) {
                const unsigned char c = static_cast<unsigned char>(h[i]);
                return std::isalnum(c) || c == '_' || c >= 0x80;
            };
            const bool before = pos == 0 || !isWord(pos - 1);
            const bool after = pos + n.size() >= h.size() || !isWord(pos + n.size());
            if (!word || (before && after)) return true;
            ++pos;
        }
        return false;
    };
}

Pred Rx(const std::string& pattern, bool icase, bool word = false) {
    auto re = std::make_shared<std::regex>(word ? "\\b(?:" + pattern + ")\\b" : pattern, icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
    return [re](const std::string& s) { return std::regex_search(s, *re); };
}

struct Sorted {
    std::vector<std::tuple<std::string, uint32_t, std::string>> rows;  // lower display, line, text
    void Add(const std::string& display, uint32_t line, const std::string& text) {
        rows.emplace_back(LowerAscii(display), line, display + ":" + std::to_string(line) + ":" + text);
    }
    std::vector<std::string> Lines() {
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
            return std::get<1>(a) < std::get<1>(b);
        });
        std::vector<std::string> out;
        for (auto& r : rows) out.push_back(std::get<2>(r));
        return out;
    }
};

std::vector<std::string> Expected(const Pred& pred, bool otherText, const std::string& under = std::string()) {
    Sorted s;
    for (const Doc& d : g_docs) {
        if (d.text && !otherText) continue;
        if (!otherText && d.display.rfind("meshes\\", 0) == 0) continue;  // not descended into without other text
        if (!under.empty() && d.display.rfind(under, 0) != 0) continue;
        for (size_t i = 0; i < d.lines.size(); ++i)
            if (pred(d.lines[i])) s.Add(d.display, static_cast<uint32_t>(i + 1), d.lines[i]);
    }
    return s.Lines();
}

struct Result {
    bool started = false;
    std::vector<std::string> lines;
    std::vector<TextSearchHit> hits;
    std::vector<TextSearchFile> files;
    TextSearchStats stats;
};

Result Gather(TextSearch& ts, bool started) {
    Result r;
    r.started = started;
    ts.TakeHits(r.hits);
    r.files = ts.Files();
    r.stats = ts.Stats();
    Sorted s;
    for (const TextSearchHit& h : r.hits) s.Add(Utf8(r.files[h.file].display), h.line, Utf8(h.text));
    r.lines = s.Lines();
    return r;
}

Result Run(const fs::path& root, const TextSearchOptions& o) {
    TextSearch ts;
    const bool started = ts.Start(root.wstring(), o);
    ts.Wait();
    return Gather(ts, started);
}

TextSearchOptions Opts(const std::string& q, bool matchCase = false, bool word = false, bool regex = false, bool other = false, int threads = 4) {
    TextSearchOptions o;
    o.query = Wide(q);
    o.matchCase = matchCase;
    o.wholeWord = word;
    o.regex = regex;
    o.otherText = other;
    if (other) o.extraExtensions = ParseExtensionList(DefaultExtraExtensionsText());
    o.threads = threads;
    return o;
}

void Compare(const std::string& name, const Result& r, const std::vector<std::string>& expected) {
    const bool ok = r.started && r.lines == expected;
    std::string detail;
    if (!ok) {
        detail = "got " + std::to_string(r.lines.size()) + " expected " + std::to_string(expected.size());
        if (!r.started) detail += " (didn't start: " + Utf8(r.stats.firstError) + ")";
        for (size_t i = 0; i < std::max(r.lines.size(), expected.size()); ++i) {
            const std::string a = i < r.lines.size() ? r.lines[i] : "<none>", b = i < expected.size() ? expected[i] : "<none>";
            if (a != b) {
                detail += "; first difference: got [" + a + "] expected [" + b + "]";
                break;
            }
        }
    }
    Check(name, ok, detail);
}

// ---------------------------------------------------------------- processes

std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

struct Proc {
    DWORD code = 99999;
    std::string out, err;
};

Proc RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args) {
    Proc p;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR, outW, errR, errW;
    CreatePipe(&outR, &outW, &sa, 0);
    CreatePipe(&errR, &errW, &sa, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = Quote(exe);
    for (const std::wstring& a : args) cmd += L" " + Quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        return p;
    }
    auto drain = [](HANDLE h, std::string* into) {
        char buf[65536];
        DWORD n;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) into->append(buf, n);
    };
    std::thread t([&] { drain(errR, &p.err); });
    drain(outR, &p.out);
    t.join();
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &p.code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outR);
    CloseHandle(errR);
    return p;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// ---------------------------------------------------------------- RequiredLiteral

void TestRequiredLiteral() {
    struct Case {
        const char* pattern;
        const char* expected;
    };
    const Case cases[] = {
        {"Debug\\.Trace", "Debug.Trace"},
        {"foo|barbaz", ""},
        {"abc", "abc"},
        {"ab", ""},
        {"colou?r", "colo"},
        {"foo(bar)+baz", "foo"},
        {"a.*longliteral.*b", "longliteral"},
        {"[abc]+xyz", "xyz"},
        {"\\d{3}", ""},
        {"x{0,2}abcd", "abcd"},
        {"abc+def", "abc"},
        {"(foo|bar)baz", "baz"},
        {"abc|def", ""},
        {"(?i)hello", "hello"},
        {"\\Qa.b\\E", ""},
        {"foo\\bbar", "foo"},
        {"wor[kd]er", "wor"},
        {"Debug\\.Trace\\(\\s*\"", "Debug.Trace("},
        {"a(b|c)d", ""},
        {"(?x) abc def", ""},
        {"\\x41BCD", "BCD"},
        {"abcd*", "abc"},
        {"abc{2,}", "abc"},
        {"foo\\.\\(bar", "foo.(bar"},
        {"[a-z]+(abc)?", ""},
        {"abc\\|def", "abc|def"},
        {"x*abcdef?g", "abcde"},
        {"ab+?cde", "cde"},
        {"Caf\xC3\xA9\\d", "Caf\xC3\xA9"},
        {"^abc$", "abc"},
        {"[]abc]xyz12", "xyz12"},
        {"ab{,2}cde", "cde"},
        {"((a|b)c)defg", "defg"},
        {"a\\.b\\.c", "a.b.c"},
        {"abc(?:def)ghi", "abc"},
        // PCRE2 10.43+: spaces inside the braces
        {"abc{ 2 }", "abc"},
        {"abcd{ 0 }", "abc"},
        {"abcd{ ,3}", "abc"},
        {"abc{2, 3}", "abc"},
        {"abcd{ 2 , 3 }e", "abcd"},
        {"abcd{ x}", "abc"},
        {"ab{x}cde", "ab{x}cde"},
        {"x{ 0, 2 }abcd", "abcd"},
        // \Q...\E inside a group or a class
        {"(?:\\Q)\\Efoo)?bar", "bar"},
        {"[\\Q]\\Eabc]xyz", "xyz"},
        {"(\\Q(\\E)?abcd", "abcd"},
    };
    int n = 0;
    for (const Case& c : cases) {
        const std::string got = RequiredLiteral(c.pattern);
        Check(std::string("RequiredLiteral ") + std::to_string(++n) + " [" + c.pattern + "]", got == c.expected, "got [" + got + "] expected [" + c.expected + "]");
    }
}

// ---------------------------------------------------------------- the main fixture

fs::path g_cli;

struct Fixture {
    fs::path main;
    std::string bsaRootBytes, bsaCompBytes, bsaOldBytes;
};

Fixture BuildMain(const fs::path& base) {
    Fixture f;
    f.main = base / L"main";
    const fs::path& m = f.main;
    AddLoose(m, {"Scripts\\Source\\A.psc", {"Scriptname A", "Zebra one", "int zebraCount", "zebra two", "Debug.Trace(\"zebra\")", "zebra zebra"}, "\r\n"});
    AddLoose(m, {"Source\\Scripts\\B.psc", {"Scriptname B", "; zebra in B", "Debug.Trace( \"x\" )"}, "\n"});
    AddLoose(m, {"Source\\C.psc", {"zebra at start of C"}, "\n", false});
    AddLoose(m, {"Scripts\\D.psc", {"nothing here", "Zebra"}, "\r\n", false});
    AddLoose(m, {"Scripts\\Num.psc", {"no digits", "value 123 here", "x12y", "4567", "a1b2c3", "zebra 99"}, "\n"});
    AddLoose(m, {"Scripts\\Uni.psc", {"\xC3\xA9\xE2\x82\xAC zebra z", "\xF0\x9F\x98\x80 zebra", "\ttab\tzebra"}, "\n"});
    AddLoose(m, {"meshes\\E.psc", {"zebra in meshes"}, "\n"});
    AddLoose(m, {"Data.txt", {"zebra in txt", "second"}, "\n"});
    AddLoose(m, {"cafe1252.txt", {"Caf\xC3\xA9 au lait", "zebra 1252"}, "\r\n", true, Enc::Cp1252});
    AddLoose(m, {"cafe8.txt", {"Caf\xC3\xA9 noir"}, "\n"});
    AddLoose(m, {"utf16.txt", {"zebra utf16", "second Caf\xC3\xA9"}, "\r\n", true, Enc::Utf16});
    AddLoose(m, {"bom8.txt", {"zebra bom", "next"}, "\r\n", true, Enc::Utf8Bom});
    AddLoose(m, {"empty.txt", {}, "\n"});
    WriteFileBytes(m / L"bin.txt", std::string("zebra\0binary", 12));
    WriteFileBytes(m / L"skipme.dds", "zebra");  // not a text type

    f.bsaRootBytes = AddBsa(m, "Root.bsa", 105, false,
                            {{"scripts\\source\\r1.psc", {"Scriptname r1", "zebra in r1"}, "\r\n"},
                             {"source\\scripts\\r2.psc", {"zebra r2"}, "\n", false},
                             {"interface\\translations\\t.txt", {"zebra tr", "$KEY\tValue"}, "\r\n", true, Enc::Utf16}},
                            {{"textures\\x.dds", std::string("zebra\0dds", 9)}});
    f.bsaCompBytes = AddBsa(m, "Comp105.bsa", 105, true,
                            {{"scripts\\source\\c1.psc", {"c1", "line zebra"}, "\n"}, {"scripts\\c3.psc", {"Zebra c3 long long long long long long long long long long long long long"}, "\n"}});
    f.bsaOldBytes = AddBsa(m, "Sub\\Old104.bsa", 104, true,
                           {{"scripts\\source\\o1.psc", {"zebra old"}, "\n"}, {"source\\scripts\\o2.psc", {"a", "b", "ZEBRA o2"}, "\r\n", false}});
    AddBsa(m, "Sub\\Deeper\\Plain104.bsa", 104, false, {{"scripts\\p1.psc", {"p1 zebra"}, "\n"}});
    return f;
}

void TestMain(const Fixture& f) {
    const fs::path& m = f.main;
    for (int mode = 0; mode < 2; ++mode) {
        const bool other = mode == 1;
        const std::string tag = other ? " (other text)" : "";
        Compare("literal, case off" + tag, Run(m, Opts("zebra", false, false, false, other)), Expected(Literal("zebra", false, false), other));
        Compare("literal, match case" + tag, Run(m, Opts("zebra", true, false, false, other)), Expected(Literal("zebra", true, false), other));
        Compare("literal, whole word" + tag, Run(m, Opts("zebra", false, true, false, other)), Expected(Literal("zebra", false, true), other));
        Compare("literal, whole word + case" + tag, Run(m, Opts("zebra", true, true, false, other)), Expected(Literal("zebra", true, true), other));
        Compare("regex with a required literal" + tag, Run(m, Opts("zebra\\s+(one|two)", false, false, true, other)), Expected(Rx("zebra\\s+(one|two)", true), other));
        Compare("regex, no literal (\\d{3})" + tag, Run(m, Opts("\\d{3}", false, false, true, other)), Expected(Rx("\\d{3}", false), other));
        Compare("regex, top-level alternation" + tag, Run(m, Opts("zebra two|Count", false, false, true, other)), Expected(Rx("zebra two|Count", true), other));
        Compare("regex, ^ anchor" + tag, Run(m, Opts("^zebra", false, false, true, other)), Expected(Rx("^zebra", true), other));
        Compare("regex, $ anchor (CRLF files too)" + tag, Run(m, Opts("zebra$", false, false, true, other)), Expected(Rx("zebra$", true), other));
        Compare("regex, ^$ finds nothing here" + tag, Run(m, Opts("^$", false, false, true, other)), Expected(Rx("^$", false), other));
        Compare("regex, match case" + tag, Run(m, Opts("^Zebra", true, false, true, other)), Expected(Rx("^Zebra", false), other));
        Compare("regex, whole word" + tag, Run(m, Opts("zebra", false, true, true, other)), Expected(Rx("zebra", true, true), other));
    }
    Compare("regex (?i) with match case on", Run(m, Opts("(?i)ZEBRA t", true, false, true)), Expected(Rx("zebra t", true), false));
    Compare("regex, (?i) prefilter with literal", Run(m, Opts("(?i)ZEBRAcount", true, false, true)), Expected(Rx("zebracount", true), false));
    Compare("literal with regex metacharacters", Run(m, Opts("Debug.Trace(\"", false)), Expected(Literal("Debug.Trace(\"", false, false), false));
    Compare("regex with escaped punctuation", Run(m, Opts("Debug\\.Trace\\(\\s*\"", false, false, true)), Expected(Rx("Debug\\.Trace\\(\\s*\"", true), false));

    // Folder skipping: no .psc is found under meshes\ without other text.
    {
        const Result r = Run(m, Opts("zebra in meshes", false));
        Check("meshes\\*.psc not found by default", r.started && r.lines.empty());
        const Result r2 = Run(m, Opts("zebra in meshes", false, false, false, true));
        Check("meshes\\*.psc found with other text", r2.lines.size() == 1 && r2.lines[0] == "meshes\\E.psc:1:zebra in meshes", Join(r2.lines));
    }
    // The four standard places, display paths exactly.
    {
        const Result r = Run(m, Opts("zebra", false));
        std::vector<std::string> files;
        for (const TextSearchFile& tf : r.files) files.push_back(Utf8(tf.display));
        std::sort(files.begin(), files.end());
        const std::vector<std::string> want = {"Comp105.bsa\\scripts\\c3.psc",
                                               "Comp105.bsa\\scripts\\source\\c1.psc",
                                               "Root.bsa\\scripts\\source\\r1.psc",
                                               "Root.bsa\\source\\scripts\\r2.psc",
                                               "Scripts\\D.psc",
                                               "Scripts\\Num.psc",
                                               "Scripts\\Source\\A.psc",
                                               "Scripts\\Uni.psc",
                                               "Source\\C.psc",
                                               "Source\\Scripts\\B.psc",
                                               "Sub\\Deeper\\Plain104.bsa\\scripts\\p1.psc",
                                               "Sub\\Old104.bsa\\scripts\\source\\o1.psc",
                                               "Sub\\Old104.bsa\\source\\scripts\\o2.psc"};
        Check("display paths (4 loose places, v105, v105 compressed, v104 compressed, subfolders)", files == want, Join(files));
        Check("stats: 4 archives, no errors, nothing binary", r.stats.archivesOpened == 4 && r.stats.errors == 0 && r.stats.skippedBinary == 0 && r.stats.done && !r.stats.cancelled && !r.stats.truncated,
              "archives " + std::to_string(r.stats.archivesOpened) + " errors " + std::to_string(r.stats.errors) + " binary " + std::to_string(r.stats.skippedBinary));
        Check("stats: counts", r.stats.hits == r.hits.size() && r.stats.filesWithHits == r.files.size());
        const Result o = Run(m, Opts("zebra", false, false, false, true));
        Check("stats: binary .txt skipped and counted (loose)", o.stats.skippedBinary == 1, std::to_string(o.stats.skippedBinary));
        bool bin = false;
        for (const TextSearchFile& tf : o.files) bin = bin || Utf8(tf.display) == "bin.txt";
        Check("binary file has no hits", !bin);
    }
    // Encodings and line endings.
    {
        const Result r = Run(m, Opts("Caf\xC3\xA9", false, false, false, true));
        const std::vector<std::string> want = {"cafe1252.txt:1:Caf\xC3\xA9 au lait", "cafe8.txt:1:Caf\xC3\xA9 noir", "utf16.txt:2:second Caf\xC3\xA9"};
        Check("Cafe: 1252 file, UTF-8 file and UTF-16 file all hit; text decoded", r.lines == want, Join(r.lines));
        const Result w = Run(m, Opts("zebra", false, false, false, true));
        bool bomOk = false, utf16Ok = false;
        for (const TextSearchHit& h : w.hits) {
            if (Utf8(w.files[h.file].display) == "bom8.txt" && h.line == 1 && h.text == L"zebra bom" && h.ranges.size() == 1 && h.ranges[0].first == 0) bomOk = true;
            if (Utf8(w.files[h.file].display) == "utf16.txt" && h.line == 1 && h.text == L"zebra utf16") utf16Ok = true;
        }
        Check("UTF-8 BOM stripped (line 1, range starts at 0)", bomOk);
        Check("UTF-16LE with BOM searched", utf16Ok);
        Check("UTF-16 text inside an archive (other text)", [&] {
            for (const TextSearchFile& tf : w.files)
                if (Utf8(tf.display) == "Root.bsa\\interface\\translations\\t.txt") return true;
            return false;
        }());
        const Result lf = Run(m, Opts("zebra at start", false));
        Check("a line without a final newline is searched (line 1)", lf.lines == std::vector<std::string>{"Source\\C.psc:1:zebra at start of C"}, Join(lf.lines));
        const Result crlf = Run(m, Opts("zebra two", false));
        Check("CRLF: line number right and CR not in the text", crlf.lines == std::vector<std::string>{"Scripts\\Source\\A.psc:4:zebra two"}, Join(crlf.lines));
    }
    // Ranges in wchar_t units.
    {
        const Result r = Run(m, Opts("zebra", false));
        auto find = [&](const std::string& display, uint32_t line) -> const TextSearchHit* {
            for (const TextSearchHit& h : r.hits)
                if (Utf8(r.files[h.file].display) == display && h.line == line) return &h;
            return nullptr;
        };
        const TextSearchHit* a = find("Scripts\\Source\\A.psc", 6);
        Check("two hits in one line: one entry, two ranges", a && a->ranges.size() == 2 && a->ranges[0] == std::make_pair(0u, 5u) && a->ranges[1] == std::make_pair(6u, 11u));
        const TextSearchHit* u = find("Scripts\\Uni.psc", 1);
        Check("range after a 2-byte and a 3-byte character", u && u->ranges.size() == 1 && u->ranges[0] == std::make_pair(3u, 8u) && u->text == L"\u00E9\u20AC zebra z");
        const TextSearchHit* e = find("Scripts\\Uni.psc", 2);
        Check("range after a surrogate pair", e && e->ranges.size() == 1 && e->ranges[0] == std::make_pair(3u, 8u));
        const TextSearchHit* t = find("Scripts\\Uni.psc", 3);
        Check("tabs kept", t && t->text == L"\ttab\tzebra" && t->ranges[0] == std::make_pair(5u, 10u));
        const Result c = Run(m, Opts("Caf\xC3\xA9", false, false, false, true));
        bool ok1252 = false;
        for (const TextSearchHit& h : c.hits)
            if (Utf8(c.files[h.file].display) == "cafe1252.txt") ok1252 = h.text == L"Caf\u00E9 au lait" && h.ranges.size() == 1 && h.ranges[0] == std::make_pair(0u, 4u);
        Check("1252 line: decoded text and range", ok1252);
    }
    // Errors.
    {
        TextSearch ts;
        const bool ok = ts.Start(m.wstring(), Opts("(abc", false, false, true));
        const std::wstring e = ts.Stats().firstError;
        Check("bad regex: Start false with a message", !ok && e.rfind(L"Bad regex at ", 0) == 0, Utf8(e));
        const bool ok2 = ts.Start(m.wstring(), Opts("", false));
        Check("empty query: Start false", !ok2 && !ts.Stats().firstError.empty());
        const bool ok3 = ts.Start((m / L"nope").wstring(), Opts("x", false));
        Check("missing root: Start false", !ok3 && !ts.Stats().firstError.empty());
        const bool ok4 = ts.Start((m / L"Data.txt").wstring(), Opts("x", false));
        Check("a file that isn't a .bsa as root: Start false", !ok4);
    }
    // A .bsa as the scope.
    {
        const Result r = Run(m / L"Root.bsa", Opts("zebra", false));
        const std::vector<std::string> want = {"Root.bsa\\scripts\\source\\r1.psc:2:zebra in r1", "Root.bsa\\source\\scripts\\r2.psc:1:zebra r2"};
        Check("a .bsa as the scope", r.lines == want, Join(r.lines));
    }
    // Thread counts give the same results.
    {
        const std::vector<std::string> want = Expected(Literal("zebra", false, false), true);
        for (int t : {1, 16, 0}) {
            TextSearchOptions o = Opts("zebra", false, false, false, true, t);
            Compare("threads=" + std::to_string(t) + " (0 = the work budget)", Run(m, o), want);
        }
    }
    // maxHits.
    {
        TextSearchOptions o = Opts("zebra", false);
        o.maxHits = 3;
        const Result r = Run(m, o);
        Check("maxHits: 3 hits, truncated, not cancelled", r.hits.size() == 3 && r.stats.truncated && !r.stats.cancelled && r.stats.hits == 3,
              "hits " + std::to_string(r.hits.size()));
        TextSearchOptions o2 = Opts("zebra", false);
        o2.maxHits = Expected(Literal("zebra", false, false), false).size();
        const Result r2 = Run(m, o2);
        Check("maxHits equal to the hit count: not truncated", !r2.stats.truncated && r2.hits.size() == o2.maxHits);
    }
}

// ---------------------------------------------------------------- files that stress the reader

std::string BigLine(size_t number) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "line %08zu qwertyuiop asdfghjkl zxcvbnm 0123456789 abcdefghijklmnopqrstuvwxyz padding padding pad", number);
    std::string s(buf);
    s.resize(99, '.');
    return s;
}

constexpr size_t kBigLines = 1500000;

void WriteBig(const fs::path& path) {
    fs::create_directories(path.parent_path());
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    std::string block;
    block.reserve(9u << 20);
    for (size_t i = 1; i <= kBigLines; ++i) {
        std::string line = BigLine(i);
        if (i == 5) line.replace(10, 12, "needle_start");
        if (i == 41944) line.replace(0, 11, "needle_mid ");
        if (i == 83887) line.replace(0, 12, "needle_mid2 ");
        if (i == kBigLines) line.replace(20, 10, "needle_end");
        block += line;
        if (i != kBigLines) block.push_back('\n');
        if (block.size() > (8u << 20)) {
            std::fwrite(block.data(), 1, block.size(), f);
            block.clear();
        }
    }
    std::fwrite(block.data(), 1, block.size(), f);
    std::fclose(f);
}

void TestBig(const fs::path& base) {
    const fs::path big = base / L"big";
    const auto t0 = std::chrono::steady_clock::now();
    WriteBig(big / L"Scripts" / L"Source" / L"huge.psc");
    std::printf("      (generated %.0f MB in %.1f s)\n", static_cast<double>(fs::file_size(big / L"Scripts" / L"Source" / L"huge.psc")) / 1048576, Seconds(t0));
    auto t1 = std::chrono::steady_clock::now();
    const Result r = Run(big, Opts("needle_", true, false, false, false, 2));
    const double lit = Seconds(t1);
    const std::vector<uint32_t> want = {5, 41944, 83887, static_cast<uint32_t>(kBigLines)};
    std::vector<uint32_t> got;
    for (const TextSearchHit& h : r.hits) got.push_back(h.line);
    std::sort(got.begin(), got.end());
    Check("150 MB file: hits near the start, across chunk boundaries and at the end, exact line numbers", got == want && r.stats.errors == 0, "took " + std::to_string(lit) + " s");
    std::printf("      (literal over %.0f MB: %.2f s)\n", static_cast<double>(r.stats.bytesSearched) / 1048576, lit);
    bool endOk = false;
    for (const TextSearchHit& h : r.hits)
        if (h.line == kBigLines) endOk = h.text.find(L"needle_end") == 20 && h.ranges.size() == 1 && h.ranges[0] == std::make_pair(20u, 27u);
    Check("150 MB file: last line without a newline, range exact", endOk);
    t1 = std::chrono::steady_clock::now();
    const Result ci = Run(big, Opts("NEEDLE_END", false, false, false, false, 2));
    Check("150 MB file: case-insensitive search", ci.hits.size() == 1 && ci.hits[0].line == kBigLines, "took " + std::to_string(Seconds(t1)) + " s");
    t1 = std::chrono::steady_clock::now();
    const Result rx = Run(big, Opts("needle_(start|end)", true, false, true, false, 2));
    std::vector<uint32_t> g2;
    for (const TextSearchHit& h : rx.hits) g2.push_back(h.line);
    std::sort(g2.begin(), g2.end());
    Check("150 MB file: regex with a required literal (prefilter)", g2 == std::vector<uint32_t>{5, static_cast<uint32_t>(kBigLines)}, "took " + std::to_string(Seconds(t1)) + " s");
    const Result ww = Run(big, Opts("needle_mid", true, true, false, false, 2));
    Check("150 MB file: whole word across a chunk boundary (needle_mid vs needle_mid2)", ww.hits.size() == 1 && ww.hits[0].line == 41944);

    // One 3 MB line.
    const fs::path longDir = base / L"long";
    std::string line(1500000, 'a');
    line += "NEEDLE_L";
    line.append(1500000, 'b');
    WriteFileBytes(longDir / L"Scripts" / L"long.psc", "first\r\n" + line + "\r\nsecond line NEEDLE_L\nlast");
    const Result lr = Run(longDir, Opts("needle_l", false, false, false, false, 2));
    bool l2 = lr.hits.size() == 2;
    if (l2) {
        std::vector<TextSearchHit> h = lr.hits;
        std::sort(h.begin(), h.end(), [](const TextSearchHit& a, const TextSearchHit& b) { return a.line < b.line; });
        const TextSearchHit& first = h[0];
        l2 = first.line == 2 && first.ranges.size() == 1 && first.text.size() >= 900 && first.text.size() <= 1010 && first.text.front() == L'\u2026' &&
             first.text.back() == L'\u2026' && first.text.substr(first.ranges[0].first, 8) == L"NEEDLE_L" && h[1].line == 3;
    }
    Check("a single 3 MB line with a hit: found, text cut to ~1000 chars with ellipses, range inside the text", l2);
    const Result lrx = Run(longDir, Opts("a+NEEDLE_L", false, false, true, false, 2));
    Check("a single 3 MB line: regex", lrx.hits.size() == 1 && lrx.hits[0].line == 2, std::to_string(lrx.hits.size()));

    // Cancel.
    for (int attempt = 0; attempt < 2; ++attempt) {
        TextSearch ts;
        TextSearchOptions o = Opts("\\d{9}x", false, false, true, false, 2);  // no literal: every line runs the regex
        const bool started = ts.Start(big.wstring(), o);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto c0 = std::chrono::steady_clock::now();
        ts.Cancel();
        ts.Wait();
        const double took = Seconds(c0);
        const TextSearchStats st = ts.Stats();
        Check(std::string("cancel on the huge file returns in under 500 ms, stats.cancelled") + (attempt ? " (again)" : ""), took < 0.5 && st.cancelled && st.done,
              "started " + std::to_string(started) + " took " + std::to_string(took) + " s done " + std::to_string(st.done) + " files " + std::to_string(st.filesSearched) + " errors " + std::to_string(st.errors) + " " + Utf8(st.firstError));
    }
    {
        TextSearch ts;
        ts.Start(big.wstring(), Opts("needle_zzz", false, false, false, false, 2));  // literal, with the fold
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto c0 = std::chrono::steady_clock::now();
        ts.Cancel();
        ts.Wait();
        Check("cancel during a literal search returns in under 500 ms", Seconds(c0) < 0.5, std::to_string(Seconds(c0)));
    }
    {
        TextSearch ts;
        ts.Start(big.wstring(), Opts("needle_zzz", false, false, false, false, 0));  // the work budget
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto c0 = std::chrono::steady_clock::now();
        ts.Cancel();
        ts.Wait();
        Check("cancel with the work budget returns in under 500 ms", Seconds(c0) < 0.5, std::to_string(Seconds(c0)));
    }
}

// ---------------------------------------------------------------- scale

void TestScale(const fs::path& base) {
    const fs::path root = base / L"scale";
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 300; ++i) {
        const std::string n = std::to_string(i);
        std::vector<BsaFileSpec> specs = {{"scripts\\source\\s" + n + "_0.psc", "Scriptname s" + n + "\r\nscalemark in " + n + "\r\n"},
                                          {"scripts\\source\\s" + n + "_1.psc", "scalemark one\nplain\nSCALEMARK two\n"},
                                          {"scripts\\source\\s" + n + "_2.psc", "nothing to see " + n + "\n"},
                                          {"textures\\t" + n + ".dds", std::string("scalemark\0x", 11)}};
        const std::string bytes = BuildBsa(i % 2 ? 105 : 104, i % 3 == 0, specs);
        WriteFileBytes(root / L"bsa" / (L"Mod" + std::to_wstring(i / 30)) / (L"a" + std::to_wstring(i) + L".bsa"), bytes);
    }
    for (int d = 0; d < 50; ++d)
        for (int k = 0; k < 100; ++k) {
            const int id = d * 100 + k;
            const std::string body = (id % 10 == 0) ? "Scriptname L" + std::to_string(id) + "\nscalemark here\n" : "Scriptname L" + std::to_string(id) + "\nplain\n";
            WriteFileBytes(root / L"loose" / (L"dir" + std::to_wstring(d)) / L"Scripts" / (L"l" + std::to_wstring(id) + L".psc"), body);
        }
    std::printf("      (built 300 archives and 5000 loose files in %.1f s)\n", Seconds(t0));
    const uint64_t wantHits = 300 * 3 + 500, wantFiles = 300 * 2 + 500;
    for (int t : {16, 0}) {
        const auto t1 = std::chrono::steady_clock::now();
        const Result r = Run(root, Opts("scalemark", false, false, false, false, t));
        const double secs = Seconds(t1);
        Check("scale (300 archives + 5,000 loose files), threads=" + std::to_string(t) + ": exact counts",
              r.stats.hits == wantHits && r.stats.filesWithHits == wantFiles && r.stats.archivesOpened == 300 && r.stats.errors == 0 && r.hits.size() == wantHits,
              "hits " + std::to_string(r.stats.hits) + " files " + std::to_string(r.stats.filesWithHits) + " archives " + std::to_string(r.stats.archivesOpened) +
                  " errors " + std::to_string(r.stats.errors) + " " + Utf8(r.stats.firstError));
        std::printf("      (scale threads=%d: %.2f s, searched %llu files)\n", t, secs, static_cast<unsigned long long>(r.stats.filesSearched));
    }
}

// ---------------------------------------------------------------- pex

void TestPex(const fs::path& base) {
    const fs::path data = L"D:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition\\Data\\Scripts";
    const char* names[] = {"Debris", "sggx", "slautilscr"};
    std::string bytes[3];
    for (int i = 0; i < 3; ++i) {
        bytes[i] = ReadFileBytes(data / (Wide(names[i]) + L".pex"));
        if (bytes[i].empty()) {
            Check(std::string("--pex: sample ") + names[i] + ".pex readable from the game's Data", false, "missing");
            return;
        }
    }
    const fs::path root = base / L"pex";
    WriteFileBytes(root / L"Scripts" / L"Debris.pex", bytes[0]);
    WriteFileBytes(root / L"Scripts" / L"Source" / L"Debris.psc", "; shadow source for Debris\r\nScriptname Debris extends Form Hidden ; hand written\r\n");
    WriteFileBytes(root / L"Scripts" / L"sggx.pex", bytes[1]);
    WriteFileBytes(root / L"Pex.bsa", BuildBsa(105, true, {{"scripts\\slautilscr.pex", bytes[2]}, {"scripts\\debris.pex", bytes[0]}}));

    TextSearchOptions o = Opts("scriptname", false);
    const Result plain = Run(root, o);
    Check("pex off: only the .psc is searched", plain.lines.size() == 1 && plain.lines[0].rfind("Scripts\\Source\\Debris.psc:2:", 0) == 0 && plain.stats.pexDecompiled == 0, Join(plain.lines));
    o.decompilePex = true;
    for (int t : {1, 4, 0}) {
        o.threads = t;
        const Result r = Run(root, o);
        std::vector<std::string> shown;
        for (const std::string& l : r.lines) shown.push_back(l.substr(0, l.find(':')));
        const std::vector<std::string> want = {"Pex.bsa\\scripts\\slautilscr.pex (decompiled)", "Scripts\\sggx.pex (decompiled)", "Scripts\\Source\\Debris.psc"};
        std::sort(shown.begin(), shown.end(), [](const std::string& a, const std::string& b) { return LowerAscii(a) < LowerAscii(b); });
        Check("pex on, threads=" + std::to_string(t) + ": decompiles only the .pex without a .psc (loose and in an archive)",
              shown == want && r.stats.pexDecompiled == 2 && r.stats.errors == 0, Join(shown) + " decompiled=" + std::to_string(r.stats.pexDecompiled));
        bool text = false;
        for (const TextSearchHit& h : r.hits)
            if (Utf8(r.files[h.file].display).find("sggx.pex") != std::string::npos) text = h.text == L"Scriptname sggx extends Quest" && h.line == 1;
        if (t == 1) Check("decompiled text and line number", text);
        if (t == 1) {
            // Reading and copying a decompiled file.
            for (const TextSearchFile& tf : r.files) {
                if (!tf.decompiled) continue;
                std::string out;
                std::wstring error;
                const bool ok = ReadSearchFileBytes(tf, out, error);
                Check("ReadSearchFileBytes of a decompiled file (" + Utf8(tf.display) + ")", ok && out.rfind("\xEF\xBB\xBF" "Scriptname ", 0) == 0, Utf8(error));  // the decompiler writes a UTF-8 BOM
            }
        }
    }
}

// ---------------------------------------------------------------- reading and copying

void TestCopies(const Fixture& f, const fs::path& tempDir, const Result& r);

void TestRead(const Fixture& f, const fs::path& tempDir) {
    const fs::path& m = f.main;
    const Result r = Run(m, Opts("zebra", false));
    auto fileOf = [&](const std::string& display) -> TextSearchFile {
        for (const TextSearchFile& tf : r.files)
            if (Utf8(tf.display) == display) return tf;
        return {};
    };
    {
        const TextSearchFile tf = fileOf("Scripts\\Source\\A.psc");
        std::string bytes;
        std::wstring error;
        const bool ok = ReadSearchFileBytes(tf, bytes, error);
        Check("ReadSearchFileBytes: loose file equals the original", ok && bytes == ReadFileBytes(m / L"Scripts" / L"Source" / L"A.psc"), Utf8(error));
    }
    struct Arch {
        const char* display;
        const char* inner;
        const char* expected;
    };
    const Arch arch[] = {{"Root.bsa\\scripts\\source\\r1.psc", "scripts\\source\\r1.psc", "Scriptname r1\r\nzebra in r1\r\n"},
                         {"Comp105.bsa\\scripts\\source\\c1.psc", "scripts\\source\\c1.psc", "c1\nline zebra\n"},
                         {"Sub\\Old104.bsa\\scripts\\source\\o1.psc", "scripts\\source\\o1.psc", "zebra old\n"}};
    for (const Arch& a : arch) {
        const TextSearchFile tf = fileOf(a.display);
        std::string bytes;
        std::wstring error;
        const bool ok = ReadSearchFileBytes(tf, bytes, error);
        Check(std::string("ReadSearchFileBytes: archive entry ") + a.display, ok && bytes == a.expected && tf.inner == a.inner, Utf8(error));
    }
    // WriteSearchCopy: %TEMP% is the fixture's own folder.
    {
        const TextSearchFile tf = fileOf("Root.bsa\\scripts\\source\\r1.psc");
        std::wstring path, error;
        const bool ok = WriteSearchCopy(tf, path, error);
        const std::wstring rootHash = SearchCopyHash(tf.diskPath);
        Check("SearchCopyHash: 8 lower-case hex digits, the same for another case of the path", rootHash.size() == 8 && rootHash.find_first_not_of(L"0123456789abcdef") == std::wstring::npos && rootHash == SearchCopyHash(Wide(LowerAscii(Utf8(tf.diskPath)))), Utf8(rootHash));
        const fs::path want = tempDir / L"WHAM search" / (L"Root.bsa " + rootHash) / L"scripts" / L"source" / L"r1 (Root.bsa).psc";
        Check("WriteSearchCopy: expected path", ok && fs::path(path) == want, Utf8(path) + " " + Utf8(error));
        const DWORD attrs = GetFileAttributesW(path.c_str());
        Check("WriteSearchCopy: read-only attribute", attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY));
        Check("WriteSearchCopy: same bytes", ReadFileBytes(path) == "Scriptname r1\r\nzebra in r1\r\n");
        std::wstring again;
        Check("WriteSearchCopy: second call leaves it and returns the same path", WriteSearchCopy(tf, again, error) && again == path);
        // Different content on disk: rewritten even though read-only.
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        WriteFileBytes(fs::path(path), "changed");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
        std::wstring third;
        const bool ok3 = WriteSearchCopy(tf, third, error);
        Check("WriteSearchCopy: a different file is replaced", ok3 && ReadFileBytes(third) == "Scriptname r1\r\nzebra in r1\r\n");
        const TextSearchFile cf = fileOf("Comp105.bsa\\scripts\\source\\c1.psc");
        std::wstring cpath;
        const bool okc = WriteSearchCopy(cf, cpath, error);
        Check("WriteSearchCopy: compressed entry", okc && fs::path(cpath).filename() == L"c1 (Comp105.bsa).psc" && ReadFileBytes(cpath) == "c1\nline zebra\n");
        const TextSearchFile loose = fileOf("Scripts\\D.psc");
        std::wstring lpath;
        Check("WriteSearchCopy: a loose file is its own path", WriteSearchCopy(loose, lpath, error) && lpath == loose.diskPath);
        // Hostile names are refused.
        TextSearchFile bad = tf;
        bad.inner = "scripts\\..\\..\\evil.psc";
        std::wstring bpath;
        Check("WriteSearchCopy: refuses a path that climbs out (..)", !WriteSearchCopy(bad, bpath, error));
        bad.inner = "scripts\\ev:il.psc";
        Check("WriteSearchCopy: refuses a name with a colon", !WriteSearchCopy(bad, bpath, error));
        bad.inner = "scripts\\nul.psc";
        Check("WriteSearchCopy: refuses a device name", !WriteSearchCopy(bad, bpath, error));
        bad.inner = "\\scripts\\x.psc";
        Check("WriteSearchCopy: refuses an empty component", !WriteSearchCopy(bad, bpath, error));
        for (const char* name : {"scripts\\com0.psc", "scripts\\LPT0.txt", "scripts\\conin$.psc", "scripts\\CONOUT$", "scripts\\com\xC2\xB9.psc", "scripts\\LPT\xC2\xB3.psc", "scripts\\con .psc"}) {
            bad.inner = name;
            Check(std::string("WriteSearchCopy: refuses the device name ") + name, !WriteSearchCopy(bad, bpath, error));
        }
        // CleanUpSearchCopies: the old copy goes, the new one stays.
        SetFileAttributesW(cpath.c_str(), FILE_ATTRIBUTE_NORMAL);
        HANDLE h = CreateFileW(cpath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME old;
        GetSystemTimeAsFileTime(&old);
        ULARGE_INTEGER u;
        u.LowPart = old.dwLowDateTime;
        u.HighPart = old.dwHighDateTime;
        u.QuadPart -= 30ull * 3600 * 10000000ull;
        old.dwLowDateTime = u.LowPart;
        old.dwHighDateTime = u.HighPart;
        SetFileTime(h, nullptr, nullptr, &old);
        CloseHandle(h);
        SetFileAttributesW(cpath.c_str(), FILE_ATTRIBUTE_READONLY);
        WriteFileBytes(tempDir / L"outside.txt", "not mine");  // outside "WHAM search": must survive
        CleanUpSearchCopies();
        std::error_code ec;
        Check("CleanUpSearchCopies: the 30-hour-old copy is deleted (read-only cleared)", !fs::exists(cpath, ec));
        Check("CleanUpSearchCopies: its empty folders are removed", !fs::exists(fs::path(cpath).parent_path(), ec) && !fs::exists(tempDir / L"WHAM search" / (L"Comp105.bsa " + SearchCopyHash(cf.diskPath)), ec));
        Check("CleanUpSearchCopies: the new copy stays", fs::exists(path, ec));
        Check("CleanUpSearchCopies: nothing outside WHAM search is touched", fs::exists(tempDir / L"outside.txt", ec));
    }
    TestCopies(f, tempDir, r);
}

void TestCopies(const Fixture& f, const fs::path& tempDir, const Result& r) {
    (void)f;
    (void)r;
    // Two archives with the same name in different folders do not share a copy.
    const fs::path dup = tempDir.parent_path() / L"dup";
    const std::string a = BuildBsa(105, false, {{"scripts\\source\\dup.psc", "zebra from A\n"}});
    const std::string b = BuildBsa(105, true, {{"scripts\\source\\dup.psc", "zebra from B\n"}});
    WriteFileBytes(dup / L"A" / L"Dup.bsa", a);
    WriteFileBytes(dup / L"B" / L"Dup.bsa", b);
    const Result dr = Run(dup, Opts("zebra", false));
    std::wstring pa, pb, error;
    TextSearchFile fa, fb;
    for (const TextSearchFile& tf : dr.files) {
        if (tf.display.rfind(L"A\\", 0) == 0) fa = tf;
        else if (tf.display.rfind(L"B\\", 0) == 0) fb = tf;
    }
    const bool okA = WriteSearchCopy(fa, pa, error), okB = WriteSearchCopy(fb, pb, error);
    Check("same-named archives in two folders: two different copies, each with its own text", okA && okB && pa != pb && ReadFileBytes(pa) == "zebra from A\n" && ReadFileBytes(pb) == "zebra from B\n",
          Utf8(pa) + " | " + Utf8(pb) + " " + Utf8(error));
    Check("... in folders named <archive> <hash>", fs::path(pa).parent_path().parent_path().parent_path().filename().wstring() == L"Dup.bsa " + SearchCopyHash(fa.diskPath) &&
          fs::path(pb).parent_path().parent_path().parent_path().filename().wstring() == L"Dup.bsa " + SearchCopyHash(fb.diskPath));

    // Several writers at once (two windows, a double double-click): all succeed, the copy is whole and read-only.
    for (int round = 0; round < 5; ++round) {
        SetFileAttributesW(pa.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(pa.c_str());
        std::atomic<int> good{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&] {
                std::wstring p, e;
                if (WriteSearchCopy(fa, p, e) && p == pa) ++good;
            });
        for (std::thread& t : threads) t.join();
        const DWORD attrs = GetFileAttributesW(pa.c_str());
        Check("8 simultaneous WriteSearchCopy calls (round " + std::to_string(round + 1) + "): all succeed, whole file, read-only",
              good == 8 && ReadFileBytes(pa) == "zebra from A\n" && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY), std::to_string(good.load()));
    }
    {
        // No temp files are left behind.
        int tmp = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(tempDir / L"WHAM search", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().extension() == L".tmp") ++tmp;
        Check("no .tmp files are left in WHAM search", tmp == 0, std::to_string(tmp));
    }

    // DeleteAllSearchCopies: the whole folder goes, read-only files too, links are never followed.
    const fs::path outside = tempDir.parent_path() / L"outside_target";
    WriteFileBytes(outside / L"keep.txt", "keep me");
    const fs::path top = tempDir / L"WHAM search";
    const Proc mk = RunProcess(L"C:\\Windows\\System32\\cmd.exe", {L"/c", L"mklink", L"/J", (top / L"link").wstring(), outside.wstring()});
    Check("junction inside WHAM search created for the test", mk.code == 0, mk.out + mk.err);
    DeleteAllSearchCopies();
    std::error_code ec;
    Check("DeleteAllSearchCopies: the folder is gone (read-only copies too)", !fs::exists(top, ec));
    Check("DeleteAllSearchCopies: nothing outside it is touched, a junction's target is not entered", fs::exists(outside / L"keep.txt", ec) && fs::exists(tempDir / L"outside.txt", ec));
    DeleteAllSearchCopies();  // already gone: no harm
    Check("DeleteAllSearchCopies: a second call does nothing", !fs::exists(top, ec));
}

// ---------------------------------------------------------------- odd inputs

void TestOdd(const fs::path& base) {
    // Hostile and broken archives.
    const fs::path bad = base / L"bad";
    WriteFileBytes(bad / L"garbage.bsa", "this is not an archive at all, just text zebra");
    WriteFileBytes(bad / L"empty.bsa", "");
    const std::string good = BuildBsa(105, true, {{"scripts\\a.psc", "zebra in a\n"}});
    WriteFileBytes(bad / L"truncated.bsa", good.substr(0, good.size() / 2));
    std::string hostile = good;
    const uint32_t huge = 0xFFFFFFF0u;
    std::memcpy(&hostile[20], &huge, 4);  // file count
    WriteFileBytes(bad / L"hostile.bsa", hostile);
    std::string badLz4 = BuildBsa(105, true, {{"scripts\\b.psc", "zebra in b\n"}});
    badLz4.back() ^= 0x55;
    badLz4[badLz4.size() - 12] ^= 0x7F;
    WriteFileBytes(bad / L"damaged.bsa", badLz4);
    WriteFileBytes(bad / L"Scripts" / L"ok.psc", "zebra ok\n");
    const Result r = Run(bad, Opts("zebra", false));
    Check("broken archives: no crash, the good file is found, errors counted", r.started && r.lines.size() >= 1 && r.lines.back().find("ok.psc:1:zebra ok") != std::string::npos && r.stats.errors >= 3,
          "errors " + std::to_string(r.stats.errors) + " first: " + Utf8(r.stats.firstError) + " lines " + Join(r.lines));
    Check("broken archives: firstError names an archive", r.stats.firstError.find(L".bsa") != std::wstring::npos);

    // Junctions are not followed; hardlinks are normal files.
    const fs::path junc = base / L"junc";
    WriteFileBytes(junc / L"Real" / L"a.psc", "zebra via real\n");
    WriteFileBytes(junc / L"h1.psc", "zebra hardlink\n");
    CreateHardLinkW((junc / L"h2.psc").c_str(), (junc / L"h1.psc").c_str(), nullptr);
    const Proc mk = RunProcess(L"C:\\Windows\\System32\\cmd.exe", {L"/c", L"mklink", L"/J", (junc / L"Link").wstring(), (junc / L"Real").wstring()});
    Check("junction created for the test", mk.code == 0, mk.out + mk.err);
    const Result jr = Run(junc, Opts("zebra", false));
    const std::vector<std::string> want = {"h1.psc:1:zebra hardlink", "h2.psc:1:zebra hardlink", "Real\\a.psc:1:zebra via real"};
    Check("junction not followed, hardlinks are normal files", jr.lines == want, Join(jr.lines));
    Check("the junction is counted as a skipped linked folder", jr.stats.skippedLinked == 1, std::to_string(jr.stats.skippedLinked));
    RemoveDirectoryW((junc / L"Link").c_str());

    // Cloud-only (offline) files are not read, and are counted.
    const fs::path cloud = base / L"cloud";
    WriteFileBytes(cloud / L"Scripts" / L"here.psc", "zebra here\n");
    WriteFileBytes(cloud / L"Scripts" / L"away.psc", "zebra away\n");
    WriteFileBytes(cloud / L"Scripts" / L"away.txt", "zebra away txt\n");
    SetFileAttributesW((cloud / L"Scripts" / L"away.psc").c_str(), FILE_ATTRIBUTE_OFFLINE);
    SetFileAttributesW((cloud / L"Scripts" / L"away.txt").c_str(), FILE_ATTRIBUTE_OFFLINE);
    const Result cr = Run(cloud, Opts("zebra", false, false, false, true));
    Check("cloud-only files are skipped and counted, never read", cr.lines.size() == 1 && cr.lines[0].find("here.psc") != std::string::npos && cr.stats.skippedCloud == 2 && cr.stats.errors == 0,
          Join(cr.lines) + " skipped " + std::to_string(cr.stats.skippedCloud));

    // A file being written to by another process, and an exclusive lock.
    const fs::path lockDir = base / L"locks";
    WriteFileBytes(lockDir / L"Scripts" / L"locked.psc", "zebra locked\n");
    WriteFileBytes(lockDir / L"Scripts" / L"shared.psc", "zebra shared\n");
    HANDLE ex = CreateFileW((lockDir / L"Scripts" / L"locked.psc").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE sh = CreateFileW((lockDir / L"Scripts" / L"shared.psc").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const Result lk = Run(lockDir, Opts("zebra", false));
    CloseHandle(ex);
    CloseHandle(sh);
    Check("a file open for writing elsewhere is read; an exclusively locked one is an error", lk.lines.size() == 1 && lk.lines[0].find("shared.psc") != std::string::npos && lk.stats.errors == 1,
          Join(lk.lines) + " errors " + std::to_string(lk.stats.errors));
}

// ---------------------------------------------------------------- engine limits

// Takes WorkSlots in this process and holds them (the budget is shared by every process of the session).
struct Holder {
    std::vector<std::unique_ptr<WorkSlot>> slots;
    bool Take(int n, int timeoutMs) {
        std::atomic<bool> giveUp{false}, finished{false};
        std::thread timer([&] {
            for (int i = 0; i < timeoutMs / 10 && !finished; ++i) Sleep(10);
            giveUp = true;
        });
        for (int i = 0; i < n; ++i) {
            auto slot = std::make_unique<WorkSlot>(WorkKind::Interactive, &giveUp);
            if (!slot->Held()) break;
            slots.push_back(std::move(slot));
        }
        finished = true;
        timer.join();
        return static_cast<int>(slots.size()) == n;
    }
    void Release() { slots.clear(); }
};

bool WaitDone(TextSearch& ts, double seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    while (Seconds(t0) < seconds) {
        if (ts.Stats().done) return true;
        Sleep(10);
    }
    return ts.Stats().done;
}

void TestBudget(const Fixture& f) {
    const int limit = WorkerLimit();
    std::printf("      (work limit %d)\n", limit);
    const Result ref = Run(f.main, Opts("zebra", false));
    {
        const Result r = Run(f.main, Opts("zebra", false, false, false, false, 0));
        Check("budget mode starts max(1, WorkerLimit() - 1) workers and finds the same lines", r.stats.workers == std::max(1, limit - 1) && r.lines == ref.lines,
              "workers " + std::to_string(r.stats.workers) + " limit " + std::to_string(limit));
        const Result t3 = Run(f.main, Opts("zebra", false, false, false, false, 3));
        Check("threads=3 starts exactly 3 workers", t3.stats.workers == 3);
    }
    if (limit >= 3) {
        // Two slots held elsewhere: limit-2 free for limit-1 workers, so one worker waits for a slot for good.
        Holder h;
        if (h.Take(2, 4000)) {
            TextSearch ts;
            const bool started = ts.Start(f.main.wstring(), Opts("zebra", false, false, false, false, 0));
            const auto t0 = std::chrono::steady_clock::now();
            const bool done = WaitDone(ts, 10);
            const double took = Seconds(t0);
            Check("a worker still waiting for a slot doesn't hold the finished search back (slots stay held)", done && took < 5, "done " + std::to_string(done) + " took " + std::to_string(took) + " s");
            h.Release();
            ts.Wait();
            const Result r = Gather(ts, started);
            Check("... and it found every line", r.lines == ref.lines);
        } else {
            std::printf("      (skipped the partial-slots test: other processes hold slots)\n");
        }
    } else {
        std::printf("      (skipped the partial-slots test: it needs a work limit of 3 or more)\n");
    }
    {
        Holder h;
        if (h.Take(limit, 4000)) {
            TextSearch ts;
            const bool started = ts.Start(f.main.wstring(), Opts("zebra", false, false, false, false, 0));
            Sleep(300);
            const bool waiting = !ts.Stats().done && ts.Stats().filesSearched == 0;
            h.Release();
            const bool done = WaitDone(ts, 10);
            ts.Wait();
            const Result r = Gather(ts, started);
            Check("every slot held: the search waits, then completes once they are released", waiting && done && r.lines == ref.lines,
                  "waiting " + std::to_string(waiting) + " done " + std::to_string(done) + " lines " + std::to_string(r.lines.size()));
        } else {
            std::printf("      (skipped the all-slots test: other processes hold slots)\n");
        }
    }
    {
        Holder h;
        if (h.Take(limit, 4000)) {
            TextSearch ts;
            ts.Start(f.main.wstring(), Opts("zebra", false, false, false, false, 0));
            Sleep(100);
            const auto c0 = std::chrono::steady_clock::now();
            ts.Cancel();
            ts.Wait();
            const TextSearchStats st = ts.Stats();
            Check("cancelling a search that waits for slots returns at once", Seconds(c0) < 1.0 && st.done && st.cancelled, std::to_string(Seconds(c0)));
        }
    }
}

void TestHitBytes(const fs::path& base) {
    const fs::path root = base / L"bytes";
    std::string body;
    for (int i = 0; i < 5000; ++i) body += "zebra line " + std::to_string(i) + " with some filler text to make the line a bit longer than short\n";
    WriteFileBytes(root / L"Scripts" / L"many.psc", body);
    TextSearchOptions o = Opts("zebra", false);
    Check("maxHitBytes defaults to 64 MB, maxHits to 200,000", o.maxHitBytes == (size_t{64} << 20) && o.maxHits == 200000);
    const Result all = Run(root, o);
    Check("no byte limit hit: all 5000 lines, not truncated", all.stats.hits == 5000 && !all.stats.truncated && !all.stats.truncatedByBytes);
    o.maxHitBytes = 100000;
    const Result cut = Run(root, o);
    size_t bytes = 0;
    for (const TextSearchHit& h : cut.hits) bytes += sizeof(TextSearchHit) + h.text.size() * sizeof(wchar_t) + h.ranges.size() * 8;
    Check("maxHitBytes 100,000: stops early, truncated and truncatedByBytes, within the limit", cut.stats.truncated && cut.stats.truncatedByBytes && cut.hits.size() > 100 && cut.hits.size() < 2000 && bytes <= 100000 && cut.stats.done,
          "hits " + std::to_string(cut.hits.size()) + " bytes " + std::to_string(bytes));
    o.maxHitBytes = size_t{64} << 20;
    o.maxHits = 10;
    const Result few = Run(root, o);
    Check("maxHits 10: truncated by count, not by bytes", few.stats.truncated && !few.stats.truncatedByBytes && few.stats.hits == 10);
}

void TestRegexExtras(const fs::path& base) {
    const fs::path root = base / L"rx";
    WriteFileBytes(root / L"Scripts" / L"q.psc", "abcc here\nabc not\nabccc no\n");
    const Result r = Run(root, Opts("abc{ 2 }\\b", true, false, true));
    Check("regex abc{ 2 }\\b (spaces inside braces) finds the line with abcc", r.lines.size() == 1 && r.lines[0].find("q.psc:1:abcc here") != std::string::npos && r.stats.regexLimitLines == 0, Join(r.lines));
    const Result r2 = Run(root, Opts("abc{ 2, 3 }", true, false, true));
    Check("regex abc{ 2, 3 } finds abcc and abccc", r2.lines.size() == 2, Join(r2.lines));
    WriteFileBytes(root / L"Scripts" / L"slow.psc", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab\nfine\n");
    const Result lim = Run(root, Opts("^(a+)+$", true, false, true));
    Check("a catastrophic regex line is counted as too complex, with no hit", lim.stats.regexLimitLines == 1 && lim.stats.hits == 0 && lim.stats.done, "limit lines " + std::to_string(lim.stats.regexLimitLines) + " hits " + std::to_string(lim.stats.hits));
    if (!g_cli.empty() && fs::exists(g_cli)) {
        const Proc p = RunProcess(g_cli.wstring(), {L"-e", L"-j", L"1", L"^(a+)+$", root.wstring()});
        Check("CLI summary mentions lines too complex for the regex", p.err.find("1 lines too complex for the regex") != std::string::npos, p.err);
    }
}

// The decompiler "crashes" on the second script it is given.
std::atomic<int> g_decompileCalls{0};
void CrashOnSecond(const uint8_t*, size_t) {
    if (++g_decompileCalls == 2) *static_cast<volatile int*>(nullptr) = 1;  // an access violation
}

void TestPexCrash(const fs::path& base) {
    const fs::path data = L"D:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition\\Data\\Scripts";
    const std::string bytes = ReadFileBytes(data / L"Debris.pex");
    if (bytes.empty()) {
        Check("--pex crash: Debris.pex readable from the game's Data", false, "missing");
        return;
    }
    const fs::path root = base / L"pexcrash";
    for (int i = 0; i < 6; ++i) WriteFileBytes(root / L"Scripts" / (L"c" + std::to_wstring(i) + L".pex"), bytes);
    WriteFileBytes(root / L"Pex.bsa", BuildBsa(105, true, {{"scripts\\b1.pex", bytes}, {"scripts\\b2.pex", bytes}, {"scripts\\b3.pex", bytes}}));
    TextSearchOptions o = Opts("scriptname", false);
    o.decompilePex = true;
    for (int t : {1, 4}) {
        g_decompileCalls = 0;
        SetDecompileTestHook(&CrashOnSecond);
        o.threads = t;
        const Result r = Run(root, o);
        SetDecompileTestHook(nullptr);
        const TextSearchStats& s = r.stats;
        Check("decompiler crash, threads=" + std::to_string(t) + ": the search ends normally and the process lives", r.started && s.done && !s.cancelled);
        Check("decompiler crash, threads=" + std::to_string(t) + ": one crash, every script either decompiled, the crash or skipped",
              s.pexCrashed == 1 && s.pexDecompiled + s.pexCrashed + s.pexSkipped == 9 && s.pexDecompiled >= 1 && s.pexSkipped >= 1,
              "decompiled " + std::to_string(s.pexDecompiled) + " crashed " + std::to_string(s.pexCrashed) + " skipped " + std::to_string(s.pexSkipped));
        if (t == 1) Check("decompiler crash, threads=1: exactly one decompiled, the rest skipped", s.pexDecompiled == 1 && s.pexSkipped == 7);
        Check("decompiler crash, threads=" + std::to_string(t) + ": the message names the file; errors count it",
              s.decompileStopped.rfind(L"Decompiling stopped after ", 0) == 0 && s.decompileStopped.find(L" crashed the decompiler") != std::wstring::npos && s.firstError == s.decompileStopped && s.errors == 1,
              Utf8(s.decompileStopped));
        Check("decompiler crash, threads=" + std::to_string(t) + ": hits from before the crash are kept", r.hits.size() >= 1);
    }
    // Without the hook the same tree decompiles all nine.
    o.threads = 4;
    const Result ok = Run(root, o);
    Check("no hook: all nine decompile, nothing skipped or crashed", ok.stats.pexDecompiled == 9 && ok.stats.pexCrashed == 0 && ok.stats.pexSkipped == 0 && ok.stats.decompileStopped.empty(),
          std::to_string(ok.stats.pexDecompiled));
}

void TestPexCopies(const fs::path& base) {
    const fs::path data = L"D:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition\\Data\\Scripts";
    const std::string bytes = ReadFileBytes(data / L"sggx.pex");
    if (bytes.empty()) {
        Check("--pex copies: sggx.pex readable from the game's Data", false, "missing");
        return;
    }
    const fs::path root = base / L"pexcopies";
    WriteFileBytes(root / L"ModA" / L"Scripts" / L"sggx.pex", bytes);
    WriteFileBytes(root / L"ModB" / L"Scripts" / L"sggx.pex", bytes);
    TextSearchOptions o = Opts("scriptname", false);
    o.decompilePex = true;
    const Result r = Run(root, o);
    std::vector<std::wstring> paths;
    bool ok = r.files.size() == 2;
    for (const TextSearchFile& tf : r.files) {
        std::wstring path, error;
        ok = ok && tf.decompiled && WriteSearchCopy(tf, path, error);
        paths.push_back(path);
        const fs::path want = fs::path(L"decompiled " + SearchCopyHash(tf.diskPath)) / L"sggx (decompiled).psc";
        ok = ok && fs::path(path).parent_path().filename() == want.parent_path() && fs::path(path).filename() == want.filename();
    }
    Check("two loose sggx.pex in different folders: different decompiled copies, named decompiled <hash>\\sggx (decompiled).psc", ok && paths.size() == 2 && paths[0] != paths[1] && fs::exists(paths[0]) && fs::exists(paths[1]),
          paths.empty() ? "" : Utf8(paths[0]));
}

void TestParse() {
    const std::vector<std::wstring> got = ParseExtensionList(L".txt .ini; *.json, xml ;.TXT");
    Check("ParseExtensionList", got == std::vector<std::wstring>{L".txt", L".ini", L".json", L".xml"});
    Check("DefaultExtraExtensionsText parses to 8", ParseExtensionList(DefaultExtraExtensionsText()).size() == 8);
    Check("ParseExtensionList: empty", ParseExtensionList(L"  ;, ").empty());
}

// ---------------------------------------------------------------- the command line tool

void TestCli(const Fixture& f, const fs::path& tempDir) {
    if (g_cli.empty() || !fs::exists(g_cli)) {
        Check("wham-search.exe present", false, g_cli.string());
        return;
    }
    const std::wstring exe = g_cli.wstring();
    const std::wstring m = f.main.wstring();
    auto text = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) s += x + "\n";
        return s;
    };
    {
        const Proc p = RunProcess(exe, {L"-j", L"4", L"zebra", m});
        const std::string want = text(Expected(Literal("zebra", false, false), false));
        Check("CLI: sorted output equals the model, exit 0", p.code == 0 && p.out == want, "exit " + std::to_string(p.code) + " out " + std::to_string(p.out.size()) + " vs " + std::to_string(want.size()));
        Check("CLI: summary on stderr", p.err.find(" matching lines in ") != std::string::npos && p.err.find("archives") != std::string::npos, p.err);
    }
    {
        const Proc p = RunProcess(exe, {L"-s", L"-w", L"-t", L"zebra", m});
        Check("CLI: -s -w -t", p.code == 0 && p.out == text(Expected(Literal("zebra", true, true), true)));
    }
    {
        const Proc p = RunProcess(exe, {L"-e", L"-j", L"1", L"^zebra\\b", m});
        Check("CLI: -e", p.code == 0 && p.out == text(Expected(Rx("^zebra\\b", true), false)));
    }
    {
        const Proc p = RunProcess(exe, {L"-c", L"zebra", m});
        Check("CLI: -c prints the count", p.code == 0 && p.out == std::to_string(Expected(Literal("zebra", false, false), false).size()) + "\n", p.out);
    }
    {
        const Proc p = RunProcess(exe, {L"-l", L"zebra", m});
        const std::vector<std::string> lines = SplitLines(p.out);
        Check("CLI: -l lists each file once, sorted", p.code == 0 && lines.size() == 13 && lines.front() == "Comp105.bsa\\scripts\\c3.psc" && std::is_sorted(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) { return LowerAscii(a) < LowerAscii(b); }),
              std::to_string(lines.size()));
    }
    {
        const Proc p = RunProcess(exe, {L"--ranges", L"zebra z", m});
        Check("CLI: --ranges", p.code == 0 && p.out == "Scripts\\Source\\A.psc:6:0-7:zebra zebra\nScripts\\Uni.psc:1:3-10:\xC3\xA9\xE2\x82\xAC zebra z\n", p.out);
    }
    {
        const Proc p = RunProcess(exe, {L"--exts", L".txt", L"Caf\u00E9", m});
        Check("CLI: --exts implies -t, UTF-8 query (non-ASCII argument)", p.code == 0 && p.out == "cafe1252.txt:1:Caf\xC3\xA9 au lait\ncafe8.txt:1:Caf\xC3\xA9 noir\nutf16.txt:2:second Caf\xC3\xA9\n", p.out);
    }
    {
        const Proc p = RunProcess(exe, {L"qqqq_not_here", m});
        Check("CLI: no hits -> exit 1", p.code == 1);
    }
    {
        const Proc p = RunProcess(exe, {L"-e", L"(abc", m});
        Check("CLI: bad regex -> exit 2 and a message", p.code == 2 && p.err.find("Bad regex at ") != std::string::npos, p.err);
        const Proc q = RunProcess(exe, {L"zebra", (f.main / L"nope").wstring()});
        Check("CLI: missing folder -> exit 2", q.code == 2);
        const Proc u = RunProcess(exe, {L"--bogus", L"x", m});
        Check("CLI: unknown option -> exit 2", u.code == 2);
    }
    {
        const Proc p = RunProcess(exe, {L"--max-hits", L"2", L"zebra", m});
        Check("CLI: --max-hits 2", p.code == 0 && SplitLines(p.out).size() == 2 && p.err.find("truncated") != std::string::npos, p.err);
    }
    {
        const Proc p = RunProcess(exe, {L"--stream", L"-j", L"3", L"zebra", m});
        std::vector<std::string> lines = SplitLines(p.out);
        std::sort(lines.begin(), lines.end());
        std::vector<std::string> want = Expected(Literal("zebra", false, false), false);
        std::sort(want.begin(), want.end());
        Check("CLI: --stream prints the same lines (unsorted)", p.code == 0 && lines == want);
    }
    {
        const Proc p = RunProcess(exe, {L"--stats-json", L"-c", L"zebra", m});
        Check("CLI: --stats-json", p.err.find("{\"archivesOpened\":4") != std::string::npos && p.err.find("\"done\":true") != std::string::npos && p.err.find("\"truncated\":false") != std::string::npos, p.err);
    }
    {
        const Proc p = RunProcess(exe, {L"--copy", L"-l", L"zebra", (f.main / L"Root.bsa").wstring()});
        const fs::path copy = tempDir / L"WHAM search" / (L"Root.bsa " + SearchCopyHash((f.main / L"Root.bsa").wstring())) / L"scripts" / L"source" / L"r1 (Root.bsa).psc";
        const std::string want = "copy:Root.bsa\\scripts\\source\\r1.psc\t" + Utf8(copy.wstring()) + "\n";
        Check("CLI: --copy prints copy:<display><TAB><path> and the file exists", p.code == 0 && p.out.find(want) != std::string::npos && fs::exists(copy), p.out + p.err);
    }
}

void TestCliBig(const fs::path& base) {
    if (g_cli.empty() || !fs::exists(g_cli)) return;
    const Proc p = RunProcess(g_cli.wstring(), {L"-e", L"-j", L"2", L"--cancel-after", L"30", L"\\d{9}x", (base / L"big").wstring()});
    Check("CLI: --cancel-after 30 stops a long search, says cancelled", p.code == 1 && p.err.find("cancelled") != std::string::npos, "exit " + std::to_string(p.code) + " " + p.err);
    const Proc q = RunProcess(g_cli.wstring(), {L"-s", L"-j", L"2", L"--ranges", L"needle_end", (base / L"big").wstring()});
    Check("CLI: huge file, exact line number and range", q.code == 0 && q.out == BigLine(kBigLines).replace(20, 10, "needle_end").insert(0, "Scripts\\Source\\huge.psc:1500000:20-30:") + "\n", q.out.substr(0, 200));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool keep = false, big = true, scale = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--keep") keep = true;
        else if (a == L"--no-big") big = false;
        else if (a == L"--no-scale") scale = false;
        else g_cli = fs::absolute(a);
    }
    wchar_t tempBuf[MAX_PATH * 2];
    const DWORD tn = GetTempPathW(static_cast<DWORD>(std::size(tempBuf)), tempBuf);
    const fs::path realTemp(std::wstring(tempBuf, tn));
    const fs::path base = realTemp / (L"wham-search-test-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);
    const fs::path tempDir = base / L"tmp";
    fs::create_directories(tempDir);
    // From here on the engine's "%TEMP%\WHAM search" is inside the fixture.
    SetEnvironmentVariableW(L"TEMP", tempDir.c_str());
    SetEnvironmentVariableW(L"TMP", tempDir.c_str());
    std::printf("fixture: %s\n", base.string().c_str());

    TestRequiredLiteral();
    TestParse();
    const Fixture f = BuildMain(base);
    TestMain(f);
    TestRead(f, tempDir);
    TestOdd(base);
    TestPex(base);
    TestPexCopies(base);
    TestPexCrash(base);
    TestBudget(f);
    TestHitBytes(base);
    TestRegexExtras(base);
    TestCli(f, tempDir);
    if (scale) TestScale(base);
    if (big) {
        TestBig(base);
        TestCliBig(base);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (!keep) {
        ClearReadOnly(base);
        std::error_code ec;
        fs::remove_all(base, ec);
    } else {
        std::printf("kept %s\n", base.string().c_str());
    }
    return g_fail ? 1 : 0;
}
