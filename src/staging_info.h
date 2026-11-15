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
//             deployed, "No" when Data has nothing there. Empty elsewhere
//             (and for MO2, which deploys nothing to Data).
#pragma once

#include <string>

namespace pse {

std::wstring ModOf(const std::wstring& path);
std::wstring DeployedOf(const std::wstring& path);

}  // namespace pse
