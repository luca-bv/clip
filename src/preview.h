#pragma once
#include <filesystem>
#include <string>

// A clip's thumbnail (thumb.jpg) and filmstrip sprite (strip.jpg, kStripTiles tiles in kStripCols columns,
// evenly spaced over the clip), made once and cached so the library and the editor open instantly.
inline constexpr int kStripTiles = 60, kStripCols = 10, kTileW = 256, kTileH = 144;
// Writes both into `dir`. Returns "" or what went wrong. Slow-ish (one keyframe decode per tile): run it
// on a background thread.
std::string MakePreview(const std::filesystem::path& clip, const std::filesystem::path& dir);
