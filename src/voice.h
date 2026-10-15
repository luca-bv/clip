#pragma once
#include <functional>
#include <string>
#include <thread>

#include "common.h"

// "Hey Clip, save that": Windows' own speech recognizer, offline, with only that phrase in its grammar, so it
// can't hear anything else as the command. Lives only while a game records. Callbacks run on its thread:
// `heard` gets how many seconds ago the phrase began; `failed` gets why it can't listen.
class VoiceSave {
 public:
  VoiceSave(std::function<void(double)> heard, std::function<void(std::string)> failed);
  ~VoiceSave();

 private:
  void Loop();

  std::function<void(double)> heard_;
  std::function<void(std::string)> failed_;
  HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread thread_;
};

inline constexpr wchar_t kVoicePhrase[] = L"hey clip save that";
