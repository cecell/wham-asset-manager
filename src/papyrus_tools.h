// Decompiles .pex and compiles .psc with the skqe-papyrus-decompiler library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pse {

// Where each command writes. A decompiled script goes next to its .pex. A
// compiled script goes to the game's folder for it when the source is in
// Scripts\Source or Source\Scripts, and next to its .psc otherwise.
std::wstring DecompiledScriptPath(const std::wstring& pex);
std::wstring CompiledScriptPath(const std::wstring& psc);

// Decompiles a .pex held in memory to source text (default options, no
// resolver), for Search text. Safe to call from any thread: a hostile script
// that overflows the stack or crashes the decoder fails the call, not the
// process. False with `error` set when it can't be done; `crashed` (when given)
// says whether the failure was such a crash (the call didn't unwind: its memory
// leaked and the decoder state may be damaged) and not an ordinary decode error.
bool DecompilePexBytes(const uint8_t* data, size_t size, std::string& source, std::wstring& error, bool* crashed = nullptr);
// Tests only: `hook` runs inside the guarded call, before the decode, with the .pex
// bytes; it may fault on purpose. nullptr removes it.
void SetDecompileTestHook(void (*hook)(const uint8_t* data, size_t size));

struct PapyrusResult {
    int status = 2;  // 0 done, 1 done with warnings, 2 failed, 3 the input can't be read
    std::wstring text;
};

// One run of the command: it opens the game's script archives at most once.
class PapyrusWorkspace {
public:
    // The game's Data folder; empty finds it from the Steam install.
    explicit PapyrusWorkspace(const std::wstring& gameData = L"");
    ~PapyrusWorkspace();
    PapyrusResult Decompile(const std::wstring& pex, const std::wstring& target);
    PapyrusResult Compile(const std::wstring& psc, const std::wstring& target);

private:
    PapyrusResult DecompileUnguarded(const std::wstring& pex, const std::wstring& target);
    PapyrusResult CompileUnguarded(const std::wstring& psc, const std::wstring& target);
    PapyrusResult Guarded(bool compile, const std::wstring& input, const std::wstring& target);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pse
