#include <unknwn.h>

#include "voice.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include <atomic>
#include <chrono>

using namespace winrt::Windows::Media::SpeechRecognition;

VoiceSave::VoiceSave(std::function<void(double)> heard, std::function<void(std::string)> failed)
    : heard_(std::move(heard)), failed_(std::move(failed)), thread_(&VoiceSave::Loop, this) {}

VoiceSave::~VoiceSave() {
  SetEvent(stop_);
  thread_.join();
  CloseHandle(stop_);
}

void VoiceSave::Loop() {
  winrt::init_apartment();  // multi-threaded: the async calls below block, which an STA can't
  SpeechRecognizer recognizer{nullptr};
  try {
    // The phrase is English, so listen in English when Windows has it, whatever the system language.
    for (auto lang : SpeechRecognizer::SupportedGrammarLanguages())
      if (lang.LanguageTag().starts_with(L"en")) {
        recognizer = SpeechRecognizer(lang);
        break;
      }
    if (!recognizer) recognizer = SpeechRecognizer();
    recognizer.Constraints().Append(
        SpeechRecognitionListConstraint(winrt::single_threaded_vector<winrt::hstring>({kVoicePhrase}), L"save"));
    const auto compiled = recognizer.CompileConstraintsAsync().get();
    if (compiled.Status() != SpeechRecognitionResultStatus::Success)
      return failed_(std::format("Windows speech recognition couldn't load (status {}).", int(compiled.Status())));

    auto session = recognizer.ContinuousRecognitionSession();
    session.AutoStopSilenceTimeout(std::chrono::hours(24 * 365));
    std::atomic<int64_t> last_ms{0};
    session.ResultGenerated([&](auto&&, SpeechContinuousRecognitionResultGeneratedEventArgs const& e) {
      const auto r = e.Result();
      if (r.Status() != SpeechRecognitionResultStatus::Success ||
          (r.Confidence() != SpeechRecognitionConfidence::High && r.Confidence() != SpeechRecognitionConfidence::Medium))
        return;
      const int64_t now = GetTickCount64();
      if (now - last_ms < 3000) return;  // one sentence, one save
      last_ms = now;
      const auto ago = winrt::clock::now() - r.PhraseStartTime();
      heard_(std::clamp(std::chrono::duration<double>(ago).count(), 0.0, 10.0));
    });
    std::atomic<bool> ended{false};
    session.Completed([&](auto&&, auto&&) { ended = true; });
    session.StartAsync().get();
    Log("voice: listening for \"Hey Clip, save that\"");
    // The session can end by itself (a device change, a long silence); start it again until told to stop.
    while (WaitForSingleObject(stop_, 1000) == WAIT_TIMEOUT)
      if (ended.exchange(false)) {
        Log("voice: session ended, restarting");
        session.StartAsync().get();
      }
    try {
      session.StopAsync().get();
    } catch (...) {
    }
  } catch (const winrt::hresult_error& e) {
    const auto code = uint32_t(e.code());
    if (code == 0x80070005)  // E_ACCESSDENIED
      failed_("Windows blocked the microphone. Allow desktop apps in Settings > Privacy > Microphone.");
    else
      failed_(std::format("Voice saving isn't available: {} (0x{:08x})", winrt::to_string(e.message()), code));
  }
}
