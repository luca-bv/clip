#include <unknwn.h>

#include "discord.h"

#include <shlobj.h>
#include <urlmon.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <filesystem>
#include <fstream>

#include "common.h"
#include "games.h"

namespace json = winrt::Windows::Data::Json;

namespace {
std::string Json(const std::string& s) { return winrt::to_string(json::JsonValue::CreateStringValue(Wide(s)).Stringify()); }

constexpr uint32_t kMaxFrame = 64 * 1024;  // Discord's replies are a few KB; anything bigger isn't Discord

// One connection to Discord's IPC pipe. Frames are {op, length} little-endian, then JSON.
class Pipe {
 public:
  bool Open() {
    for (int i = 0; i < 10 && h_ == INVALID_HANDLE_VALUE; i++)
      h_ = CreateFileW(std::format(L"\\\\.\\pipe\\discord-ipc-{}", i).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                       nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    return h_ != INVALID_HANDLE_VALUE;
  }
  ~Pipe() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  // Sends a frame and reads Discord's reply to it.
  bool Call(uint32_t op, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t len = uint32_t(json.size());
    memcpy(frame.data(), &op, 4);
    memcpy(frame.data() + 4, &len, 4);
    frame += json;
    DWORD n;
    if (!WriteFile(h_, frame.data(), DWORD(frame.size()), &n, nullptr)) return false;
    uint32_t head[2];
    if (!ReadFile(h_, head, 8, &n, nullptr) || n != 8 || head[1] > kMaxFrame) return false;
    std::string body(head[1], '\0');
    for (DWORD got = 0; got < head[1]; got += n)
      if (!ReadFile(h_, body.data() + got, head[1] - got, &n, nullptr) || !n) return false;
    if (head[0] == 2) {  // close: bad app id or Discord said no
      Log("discord: {}", body);
      return false;
    }
    return true;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};
}  // namespace

DiscordPresence::DiscordPresence(std::string app_id) : app_id_(std::move(app_id)) {
  if (!app_id_.empty()) thread_ = std::thread(&DiscordPresence::Loop, this);
}

DiscordPresence::~DiscordPresence() {
  {
    std::lock_guard l(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    CancelSynchronousIo(thread_.native_handle());  // in case it's waiting on Discord's reply
    thread_.join();
  }
}

void DiscordPresence::Show(const std::string& game, const std::string& exe, int64_t since_unix) {
  {
    std::lock_guard l(m_);
    if (game == game_ && exe == exe_ && since_unix == since_) return;
    game_ = game;
    exe_ = exe;
    since_ = since_unix;
    dirty_ = true;
  }
  cv_.notify_all();
}

void DiscordPresence::Refresh() {
  {
    std::lock_guard l(m_);
    dirty_ = true;
  }
  cv_.notify_all();
}

// Clip's own icon (the one uploaded to its Discord app), for the badge on the game's icon.
void DiscordPresence::LoadIcon() {
  icon_loaded_ = true;
  const std::filesystem::path file = KnownFolder(FOLDERID_LocalAppData) / L"Clip" / L"discord-app.json";
  const std::wstring url = std::format(L"https://discord.com/api/v9/applications/{}/rpc", Wide(app_id_));
  if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), file.c_str(), 0, nullptr))) Log("discord: no app info");
  std::ifstream f(file, std::ios::binary);
  json::JsonObject o{nullptr};
  if (json::JsonObject::TryParse(Wide(std::string(std::istreambuf_iterator<char>(f), {})), o) && o.HasKey(L"icon") &&
      o.GetNamedValue(L"icon").ValueType() == json::JsonValueType::String)
    clip_icon_ = std::format("https://cdn.discordapp.com/app-icons/{}/{}.png?size=256", app_id_,
                             winrt::to_string(o.GetNamedString(L"icon")));
}

std::string DiscordPresence::Activity(const std::string& game, const std::string& exe, int64_t since) {
  if (game.empty()) return "null";
  if (!icon_loaded_) LoadIcon();
  const auto known = FindGame(exe);
  const std::string name = known ? known->name : game;
  std::string assets;
  if (known && !known->icon.empty()) {
    assets = std::format(R"("large_image":{},"large_text":{})", Json(known->icon), Json(name));
    if (!clip_icon_.empty()) assets += std::format(R"(,"small_image":{},"small_text":"Clip")", Json(clip_icon_));
  } else if (!clip_icon_.empty()) {
    assets = std::format(R"("large_image":{},"large_text":"Clip")", Json(clip_icon_));
  }
  return std::format(
      R"({{"type":0,"name":{},"details":{},"state":"with Clip","timestamps":{{"start":{}}},"assets":{{{}}},)"
      R"("buttons":[{{"label":"Get Clip on GitHub","url":"https://github.com/luca-bv/clip"}}]}})",
      Json(name + " with Clip"), Json("Clipping " + name), since, assets);
}
void DiscordPresence::Loop() {
  try {
    Run();
  } catch (const winrt::hresult_error& e) {  // presence is optional: stop it rather than take Clip down
    Log("discord: stopped: {}", winrt::to_string(e.message()));
  } catch (const std::exception& e) {
    Log("discord: stopped: {}", e.what());
  }
}

void DiscordPresence::Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  winrt::init_apartment();
  int nonce = 0;
  while (true) {
    Pipe pipe;
    bool ok = pipe.Open() && pipe.Call(0, std::format(R"({{"v":1,"client_id":{}}})", Json(app_id_)));
    if (ok) {
      Log("discord: connected");
      std::lock_guard l(m_);
      dirty_ = true;  // a new connection starts with no activity
    }
    while (ok) {
      std::string game, exe;
      int64_t since;
      {
        std::unique_lock l(m_);
        cv_.wait(l, [&] { return stop_ || dirty_; });  // a closed Discord shows up as a failed call
        if (stop_) break;
        dirty_ = false;
        game = game_;
        exe = exe_;
        since = since_;
      }
      ok = pipe.Call(1, std::format(R"({{"cmd":"SET_ACTIVITY","args":{{"pid":{},"activity":{}}},"nonce":"{}"}})",
                                    GetCurrentProcessId(), Activity(game, exe, since), ++nonce));
    }
    std::unique_lock l(m_);
    if (stop_) return;
    cv_.wait_for(l, std::chrono::seconds(15), [&] { return stop_; });  // Discord closed or not running: retry
    if (stop_) return;
  }
}