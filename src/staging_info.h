// Mod manager staging folders, for the Mod and Deployed columns.
//
// The game's Data folder and every staging folder (Vortex's, read from its
// deployment manifest in Data; MO2's instances; a folder saved with skqe
// staging --set-staging) come from staging_find, refreshed every 30 seconds.
//   Mod:      in a staging folder, the mod folder the file is in; in Data, the
//             mod folder holding another hardlink of the same file (Vortex
//             deploys hardlinks). Empty when neither.
//   Deployed: for a file in a Vortex mod: "Yes" when Data has this very file
//             (the same hardlink), "No (<mod> wins)" when Data has another
//             mod's file there, "No (a loose file wins)" for a file no mod
//             deployed, "No" when Data has nothing there. For a file in an
//             MO2 mod (MO2 deploys nothing; it shows the mods as Data while
//             the game runs): from the selected profile's modlist.txt, where
//             the first line wins: "Yes", "No (Overwrite wins)", "No (<mod>
//             wins)", "No (disabled in MO2)". Empty elsewhere.
#pragma once

#include <string>

namespace pse {

std::wstring ModOf(const std::wstring& path);
std::wstring DeployedOf(const std::wstring& path);
// "True" when the file has more than one name on disk (a hardlink: how Vortex
// and deploy scripts put files into Data), "False" when it has one, empty when
// it can't be read.
std::wstring IsHardlinkText(const std::wstring& path);

}  // namespace pse
