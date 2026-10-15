#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Discord Rich Presence over Discord's local pipe, laid out like Medal's: "<game> with Clip" on the profile, a
// card with the game's icon and Clip's as a badge, "Clipping <game>" / "with Clip", time elapsed, and a button
// to the repo. Reconnects when Discord starts or restarts; does nothing without an application id.
class DiscordPresence {
 public:
  explicit DiscordPresence(std::string app_id);
  ~DiscordPresence();
  // `exe` finds the game in Discord's list; `game` is the name to use when it isn't there. "" clears it.
  void Show(const std::string& game, const std::string& exe, int64_t since_unix);
  void Clear() { Show("", "", 0); }
  void Refresh();  // send again, e.g. once the game list has the game's icon

 private:
  void Loop();
  void LoadIcon();
  std::string Activity(const std::string& game, const std::string& exe, int64_t since);

  const std::string app_id_;
  std::mutex m_;
  std::condition_variable cv_;
  std::string game_, exe_;
  int64_t since_ = 0;
  bool dirty_ = true, stop_ = false;
  bool icon_loaded_ = false;  // loop thread only from here down
  std::string clip_icon_;
  std::thread thread_;
};
