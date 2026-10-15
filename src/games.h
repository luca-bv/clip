#pragma once
#include <functional>
#include <optional>
#include <string>

// Game names and icons from Discord's own list of games it detects (~13 MB, cached for a week), so Clip says
// "Roblox" rather than "robloxplayerbeta".
struct GameInfo {
  std::string name, icon;  // icon: https URL
};
void LoadGameList(std::function<void()> ready);           // in the background; `ready` runs on that thread
std::optional<GameInfo> FindGame(const std::string& exe);  // "robloxplayerbeta.exe" or "robloxplayerbeta"
