// Auto columns: when an Explorer window enters a folder, the folder names in
// its path pick a preset ("...\textures\..." -> Textures, "...\meshes\..." ->
// Meshes; the deepest match wins), and the window gets that preset's columns.
// A folder whose columns the user changed by hand is left alone from then on.
//
// Settings and memory: auto_columns.json next to folder_views.json:
//   enabled, rules [{folder, preset, here_only}], applied {folder: columns
//   WHAM set}, manual [folders the user set by hand].
#pragma once

#include <windows.h>

#include <shobjidl.h>

#include <string>
#include <vector>

namespace pse {

bool AutoColumnsEnabled();
void SetAutoColumnsEnabled(bool enabled);

// The preset the rules choose for `folder`, or "" when none matches.
std::wstring AutoPresetFor(const std::wstring& folder);

// The user set this folder's columns (Choose WHAM columns, a preset): auto
// columns leave it alone. ResetToAuto undoes that.
void MarkFolderManual(const std::wstring& folder);
void MarkFoldersManual(const std::vector<std::wstring>& folders);  // one write for many
bool IsFolderManual(const std::wstring& folder);
void ResetFolderToAuto(const std::wstring& folder);

// While a subfolder walk applies a preset, auto columns stay out of the way.
void SuspendAutoColumns(bool suspend);

// In explorer.exe: starts watching Explorer windows (once per process; the
// DLL then stays loaded). Does nothing in other processes.
void StartAutoColumns();

// Applies auto columns to `browser`'s current folder now, on this thread.
// Returns a short note for tests ("applied Textures", "manual", "no rule"...).
std::wstring ApplyAutoColumnsNow(IShellBrowser* browser);

}  // namespace pse
