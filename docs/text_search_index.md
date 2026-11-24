# Search text: an optional cached index (design note, not built)

Search text reads every file in scope each time (`src/text_search.cpp`). This
note describes an index that could make repeated searches of the same big
folder near-instant. It is a possible later phase, not a plan.

## Why it might be worth it

A direct search of a full Data folder (about 38,000 loose .psc and 15,800 .psc
inside 829 archives, 88 MB of text) takes several seconds. The time goes to
opening files, not to searching: plain Python reading the same 13,800 files of
`Data\Scripts` with 16 threads takes as long as the search (about 2.4 s), so it
is the per-file open cost (file system filters, antivirus). Reading all the
archives takes well under a second. An index answers from one file instead
of tens of thousands.

A trial on the same Data folder (SQLite, text stored once per content hash):

| Index | Extra size | Build | Substring query |
| --- | --- | --- | --- |
| FTS5 trigram, `detail=none` | 22 MB | 4.5 s | candidates only (no phrase queries) |
| FTS5 trigram, full detail | 167 MB | 7.4 s | 6-24 ms |
| full scan of the stored text (no FTS) | 0 | - | about 110 ms |

The last row matters: once the text sits in one file, even a plain scan
answers in about 0.1 s. Most of the win comes from not opening 54,000 files;
the trigram index is a further step.

## Shape

- **Where:** `%LOCALAPPDATA%\Plugin Shell Extension\search\<hash of the scope root>.db`,
  one file per scope root that was searched with "Remember" on (an option in
  the window, off by default). Never inside Data or a mod folder.
- **Contents:** `files(path, archive, inner, size, mtime, content_id)` and
  `contents(id, hash, text)` with text stored once per hash (vanilla scripts
  repeat across mods and archives). Optionally an FTS5 trigram table with
  `detail=none` over `contents` to pick candidates.
- **Search:** candidates from the index (or a scan of `contents`), then the
  existing engine's matcher on those texts only, so match case, whole word
  and regex behave exactly as today. The regex prefilter literal maps to the
  trigram query.
- **Kept current:** each search still lists the folders (cheap: the listing of
  Data takes a fraction of a second; it's the opens that cost) and compares
  size and modified time of every loose file and archive with the table.
  Changed or new files are re-read; missing ones are dropped. An archive whose
  size or time changed is re-read whole. So results are never stale, and the
  cost of a search becomes "list + read what changed".
- **Size:** about 82 MB of distinct text (49,500 contents) for the Data folder above,
  plus 22 MB for a `detail=none` trigram index. Stored compressed per row it
  would be roughly a third.
- **Dependencies:** SQLite (public domain amalgamation, fetched pinned like the
  other libraries). Nothing else.

## Open questions

- Whether the remaining few seconds are worth a 100 MB cache per root. A
  first search would be as slow as today, and Vortex redeploys touch many
  modified times.
- Writing the database from inside explorer.exe: it should happen on the
  search's own threads with a WorkSlot, and the file must never be left
  locked when the window closes.
- Decompiled .pex text could be cached the same way (decompiling is the
  slowest part of that option).
