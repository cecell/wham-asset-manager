// wham-search: the Search text engine from the command line.
//   wham-search [options] <pattern> <folder-or-bsa>
// Output is UTF-8. Exit 0 = hits, 1 = none, 2 = error.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "text_search.h"

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void Usage() {
    std::fputs(
        "usage: wham-search [options] <pattern> <folder-or-bsa>\n"
        "  -s, --case         match case\n"
        "  -w, --word         whole word\n"
        "  -e, --regex        pattern is a regular expression (PCRE2, per line)\n"
        "  -t, --text         also search other text types (.txt .ini .json .xml .yaml .toml .lst .csv)\n"
        "      --exts \"..\"    custom list of other text types (implies -t)\n"
        "      --pex          also decompile .pex that have no .psc in scope\n"
        "  -j, --threads N    exactly N threads, no work budget (default: the work budget)\n"
        "  -c, --count        print only the number of matching lines\n"
        "  -l, --files        print only the names of files with hits\n"
        "      --stream       print hits as found, unsorted\n"
        "      --ranges       add the hit ranges: <file>:<line>:<s>-<e>,..:<text>\n"
        "      --max-hits N   stop after N matching lines\n"
        "      --cancel-after MS  cancel after MS milliseconds (for tests)\n"
        "      --copy         write the opened copy of each file with hits, print copy:<file><TAB><path>\n"
        "      --stats-json   print the final stats as one JSON line on stderr\n"
        "output: <file>:<line>:<text>; exit 0 = hits, 1 = none, 2 = error\n",
        stderr);
}

struct Options {
    pse::TextSearchOptions search;
    std::wstring root;
    bool count = false, files = false, stream = false, ranges = false, copy = false, statsJson = false;
    int cancelAfter = -1;
};

bool ParseArgs(int argc, wchar_t** argv, Options& o) {
    std::vector<std::wstring> positional;
    bool text = false;
    std::wstring exts;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"-s" || a == L"--case") o.search.matchCase = true;
        else if (a == L"-w" || a == L"--word") o.search.wholeWord = true;
        else if (a == L"-e" || a == L"--regex") o.search.regex = true;
        else if (a == L"-t" || a == L"--text") text = true;
        else if (a == L"--exts") {
            if (!value(exts)) return false;
            text = true;
        } else if (a == L"--pex") o.search.decompilePex = true;
        else if (a == L"-j" || a == L"--threads") {
            if (!value(v)) return false;
            o.search.threads = _wtoi(v.c_str());
        } else if (a == L"-c" || a == L"--count") o.count = true;
        else if (a == L"-l" || a == L"--files") o.files = true;
        else if (a == L"--stream") o.stream = true;
        else if (a == L"--ranges") o.ranges = true;
        else if (a == L"--max-hits") {
            if (!value(v)) return false;
            o.search.maxHits = static_cast<size_t>(_wtoi64(v.c_str()));
        } else if (a == L"--cancel-after") {
            if (!value(v)) return false;
            o.cancelAfter = _wtoi(v.c_str());
        } else if (a == L"--copy") o.copy = true;
        else if (a == L"--stats-json") o.statsJson = true;
        else if (a == L"--") {
            for (++i; i < argc; ++i) positional.push_back(argv[i]);
        } else if (a.size() > 1 && a[0] == L'-' && a != L"-") {
            std::fwprintf(stderr, L"unknown option %s\n", a.c_str());
            return false;
        } else positional.push_back(a);
    }
    if (positional.size() != 2) return false;
    o.search.query = positional[0];
    o.root = positional[1];
    if (text) {
        o.search.otherText = true;
        o.search.extraExtensions = pse::ParseExtensionList(exts.empty() ? pse::DefaultExtraExtensionsText() : exts);
    }
    return true;
}

struct Line {
    std::wstring display;
    std::wstring sortKey;
    uint32_t file = 0;
    pse::TextSearchHit hit;
};

void PrintHit(const Options& o, const std::wstring& display, const pse::TextSearchHit& h) {
    std::wstring line = display + L":" + std::to_wstring(h.line) + L":";
    if (o.ranges) {
        for (size_t i = 0; i < h.ranges.size(); ++i) {
            if (i) line += L",";
            line += std::to_wstring(h.ranges[i].first) + L"-" + std::to_wstring(h.ranges[i].second);
        }
        line += L":";
    }
    line += h.text;
    line += L"\n";
    const std::string bytes = Utf8(line);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options o;
    if (!ParseArgs(argc, argv, o)) {
        Usage();
        return 2;
    }
    _setmode(_fileno(stdout), _O_BINARY);

    pse::TextSearch search;
    if (!search.Start(o.root, o.search)) {
        const std::string e = Utf8(search.Stats().firstError);
        std::fprintf(stderr, "error: %s\n", e.c_str());
        return 2;
    }

    std::vector<Line> lines;
    std::vector<pse::TextSearchHit> batch;
    uint64_t counted = 0;
    std::vector<pse::TextSearchFile> known;  // for streaming
    auto take = [&] {
        batch.clear();
        search.TakeHits(batch);
        for (pse::TextSearchHit& h : batch) {
            ++counted;
            if (o.count) continue;
            if (o.stream && !o.files) {
                while (h.file >= known.size()) known.push_back(search.File(static_cast<uint32_t>(known.size())));
                PrintHit(o, known[h.file].display, h);
                continue;
            }
            Line l;
            l.file = h.file;
            l.hit = std::move(h);
            lines.push_back(std::move(l));
        }
    };

    const auto began = std::chrono::steady_clock::now();
    bool cancelled = false;
    while (!search.Stats().done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        take();
        if (o.cancelAfter >= 0 && !cancelled && std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(o.cancelAfter)) {
            search.Cancel();
            cancelled = true;
        }
    }
    search.Wait();
    take();
    std::fflush(stdout);

    const pse::TextSearchStats st = search.Stats();
    const std::vector<pse::TextSearchFile> files = search.Files();

    if (o.count) {
        std::printf("%llu\n", static_cast<unsigned long long>(counted));
    } else if (o.files) {
        std::vector<std::wstring> names;
        for (const pse::TextSearchFile& f : files) names.push_back(f.display);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        for (const std::wstring& n : names) {
            const std::string b = Utf8(n + L"\n");
            std::fwrite(b.data(), 1, b.size(), stdout);
        }
    } else if (!o.stream) {
        for (Line& l : lines) {
            l.display = files[l.file].display;
            l.sortKey = l.display;
            std::transform(l.sortKey.begin(), l.sortKey.end(), l.sortKey.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        }
        std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
            if (a.sortKey != b.sortKey) return a.sortKey < b.sortKey;
            return a.hit.line < b.hit.line;
        });
        for (const Line& l : lines) PrintHit(o, l.display, l.hit);
    }

    if (o.copy) {
        for (const pse::TextSearchFile& f : files) {
            std::wstring path, error;
            if (pse::WriteSearchCopy(f, path, error)) {
                const std::string b = Utf8(L"copy:" + f.display + L"\t" + path + L"\n");
                std::fwrite(b.data(), 1, b.size(), stdout);
            } else {
                const std::string b = Utf8(L"copy-failed:" + f.display + L"\t" + error);
                std::fprintf(stderr, "%s\n", b.c_str());
            }
        }
    }
    std::fflush(stdout);

    std::string summary = std::to_string(st.hits) + " matching lines in " + std::to_string(st.filesWithHits) + " files; searched " +
                          std::to_string(st.filesSearched) + " files (" + std::to_string(st.bytesSearched / 1048576) + " MB), " +
                          std::to_string(st.archivesOpened) + " archives, " + std::to_string(st.pexDecompiled) + " decompiled; ";
    char secs[32];
    std::snprintf(secs, sizeof(secs), "%.2f s", st.seconds);
    summary += secs;
    if (st.skippedBinary) summary += "; " + std::to_string(st.skippedBinary) + " binary files skipped";
    if (st.errors) summary += "; " + std::to_string(st.errors) + " errors (first: " + Utf8(st.firstError) + ")";
    if (st.truncated) summary += "; truncated at the hit limit";
    if (st.cancelled) summary += "; cancelled";
    std::fprintf(stderr, "%s\n", summary.c_str());

    if (o.statsJson) {
        nlohmann::json j;
        j["filesSearched"] = st.filesSearched;
        j["bytesSearched"] = st.bytesSearched;
        j["archivesOpened"] = st.archivesOpened;
        j["foldersListed"] = st.foldersListed;
        j["filesWithHits"] = st.filesWithHits;
        j["hits"] = st.hits;
        j["pexDecompiled"] = st.pexDecompiled;
        j["skippedBinary"] = st.skippedBinary;
        j["errors"] = st.errors;
        j["seconds"] = st.seconds;
        j["done"] = st.done;
        j["cancelled"] = st.cancelled;
        j["truncated"] = st.truncated;
        j["firstError"] = Utf8(st.firstError);
        const std::string text = j.dump();
        std::fprintf(stderr, "%s\n", text.c_str());
    }

    if (st.hits > 0) return 0;
    return st.errors > 0 ? 2 : 1;
}
