// "Search text": finds text in loose files and inside .bsa archives (read in
// memory with bsa-reader, never extracted), with no UI, so the Explorer window,
// the wham-search command line tool and the tests share one engine.
//
// Scope: a folder (every loose file under it plus every .bsa under it) or a
// single .bsa. File set: .psc by default, plus the extra extensions when
// otherText is on, plus (optional) .pex decompiled in memory for scripts that
// have no .psc anywhere in scope.
//
// Threads: workers pull from one shared queue of folders, files and archives,
// so listing and searching overlap. Each worker holds a WorkSlot (unless
// threads > 0, for the command line tool) and calls Pace() between items.
// In that budget mode a search starts max(1, WorkerLimit() - 1) workers, so one
// slot stays free for Background work (columns, BSA folders) in every process.
// A worker that can't get a slot gives up as soon as the search is finished
// (done, cancelled or at a limit), so `done` doesn't wait for other jobs.
// Destroying a TextSearch waits for its workers: from a UI thread, Cancel() it
// and destroy it on another thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pse {

struct TextSearchOptions {
    std::wstring query;      // what to find; for regex, a PCRE2 pattern
    bool matchCase = false;  // off: ASCII letters match either case
    bool wholeWord = false;  // the hit must not touch [A-Za-z0-9_] on either side
    bool regex = false;
    bool otherText = false;  // also search extraExtensions
    std::vector<std::wstring> extraExtensions;  // ".txt", ".ini", ... (lower case, with the dot)
    bool decompilePex = false;  // also .pex whose script has no .psc in scope
    // 0: max(1, WorkerLimit() - 1) workers, each holding a WorkSlot (Interactive) and pacing.
    // >0: exactly this many workers (clamped to 1..16), no WorkSlot, no pacing.
    int threads = 0;
    size_t maxHits = 200000;  // stop adding lines after this many (stats.truncated)
    size_t maxHitBytes = size_t{64} << 20;  // ... or after the lines' text and ranges take this much memory (truncated + truncatedByBytes)
};

// The default extra extensions, as shown in the window's mask box.
std::wstring DefaultExtraExtensionsText();  // L".txt .ini .json .xml .yaml .toml .lst .csv"
// Parses ".txt .ini; *.json, xml" into {".txt", ".ini", ".json", ".xml"}.
std::vector<std::wstring> ParseExtensionList(const std::wstring& text);

// One file that holds at least one hit.
struct TextSearchFile {
    std::wstring display;  // relative to the scope root: "Scripts\Source\Foo.psc", "SimplyKnock.bsa\scripts\source\foo.psc",
                           // "Sub\X.bsa\scripts\foo.pex (decompiled)"; for a .bsa scope: "SimplyKnock.bsa\scripts\..."
    std::wstring diskPath;  // loose file: its full path; archive entry: the full path of the .bsa
    std::string inner;      // archive entry: its path inside the archive as stored (bytes); empty for a loose file
    bool decompiled = false;  // the text searched was decompiled from a .pex
};

// One matching line.
struct TextSearchHit {
    uint32_t file = 0;  // index into TextSearch::Files()
    uint32_t line = 1;  // 1-based
    std::wstring text;  // the line, decoded (UTF-8 if valid, else Windows-1252), tabs kept, CR/LF stripped,
                        // cut to about 1,000 characters around the first hit ("…" marks a cut)
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // [start, end) of each hit in `text`, in wchar_t units
};

struct TextSearchStats {
    uint64_t filesSearched = 0;     // loose files and archive entries whose text was searched
    uint64_t bytesSearched = 0;
    uint64_t archivesOpened = 0;
    uint64_t foldersListed = 0;
    uint64_t filesWithHits = 0;
    uint64_t hits = 0;              // matching lines
    uint64_t pexDecompiled = 0;
    uint64_t skippedBinary = 0;     // files in the set that looked binary (NUL bytes) and were skipped
    uint64_t errors = 0;            // files or archives that couldn't be read
    uint64_t pexCrashed = 0;        // decompiler crashes (the first one stops decompiling for the rest of the search)
    uint64_t pexSkipped = 0;        // .pex not decompiled because of that
    uint64_t regexLimitLines = 0;   // lines where the regex hit its match, depth or heap limit (counted as no match)
    uint64_t skippedCloud = 0;      // files that would be searched but are cloud-only / offline (reading would download them)
    uint64_t skippedLinked = 0;     // folders that are junctions or symbolic links (not followed)
    int workers = 0;                // threads started
    double seconds = 0;             // since Start
    bool done = false, cancelled = false, truncated = false;
    bool truncatedByBytes = false;  // truncated because of maxHitBytes
    std::wstring decompileStopped;  // "Decompiling stopped after <file> crashed the decompiler", or empty
    std::wstring firstError;        // e.g. "Bad regex: missing ) at 7", or the first unreadable file
};

class TextSearch {
public:
    TextSearch();
    ~TextSearch();  // cancels and waits
    TextSearch(const TextSearch&) = delete;
    TextSearch& operator=(const TextSearch&) = delete;

    // Starts on background threads and returns at once. False (with
    // Stats().firstError) for a bad pattern, an empty query or a missing root.
    bool Start(const std::wstring& root, const TextSearchOptions& options);
    void Cancel();
    void Wait();

    // Moves the hits found since the last call into `out` (appends; arrival order).
    // Files() only grows, so indexes in earlier hits stay valid.
    size_t TakeHits(std::vector<TextSearchHit>& out);
    std::vector<TextSearchFile> Files() const;  // a copy
    TextSearchFile File(uint32_t index) const;
    TextSearchStats Stats() const;
    const std::wstring& Root() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The text of one file in the results, as the editor should see it: the loose
// file's bytes, the archive entry's bytes, or the decompiled source. False with
// `error` set when it can't be read.
bool ReadSearchFileBytes(const TextSearchFile& file, std::string& bytes, std::wstring& error);

// Where the opened copy of an archive entry or a decompiled script goes:
// %TEMP%\WHAM search\<archive name> <hash>\<inner path with \>, with the archive
// name added to the file name, e.g.
// ...\SimplyKnock.bsa 1a2b3c4d\scripts\source\foo (SimplyKnock.bsa).psc; a loose
// decompiled script goes to ...\decompiled <hash>\foo (decompiled).psc. <hash> is
// SearchCopyHash() of the archive's (or the .pex's) full path, so the same name in
// two folders never shares a copy. Written atomically (temp file + rename), then
// made read-only; kept when it already holds the same bytes. Returns its path.
// Copies older than a day are deleted by CleanUpSearchCopies().
bool WriteSearchCopy(const TextSearchFile& file, std::wstring& path, std::wstring& error);
void CleanUpSearchCopies();
// 8 hex digits: FNV-1a 32 over the UTF-16 bytes of the lower-cased path.
std::wstring SearchCopyHash(const std::wstring& fullPath);
// Deletes the whole %TEMP%\WHAM search folder (read-only cleared, links never followed,
// nothing outside it); setup's uninstaller calls it through DllInstall.
void DeleteAllSearchCopies();

// Notepad++ if installed (App Paths, HKLM\SOFTWARE\Notepad++, Program Files), else empty.
std::wstring FindNotepadPlusPlus();
// Opens `path` at `line`: Notepad++ -n<line>, else the system notepad.exe. Never the
// shell's "open" verb: a hit can be any file the mask allows, and "open" would run some.
bool OpenInEditor(const std::wstring& path, uint32_t line, std::wstring& error);

// The longest run of characters (3 or more) that every match of a regex must
// contain, for the prefilter; empty when there is none. Exposed for the tests.
std::string RequiredLiteral(const std::string& utf8Pattern);

}  // namespace pse
