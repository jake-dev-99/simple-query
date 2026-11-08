// Native tests for the plugin as one engine sees it, through a fake
// messenger.

#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "native_query.g.h"
#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// The plugin's smallest polling interval.
constexpr int64_t kPluginFastPollMs = 250;
constexpr int64_t kPluginLongPollMs = 600000;

const std::string kHostChannelPrefix =
    "dev.flutter.pigeon.simple_query_windows.NativeQueryHostApi.";
const std::string kObserveStartChannel = kHostChannelPrefix + "observeStart";
const std::string kObserveEventChannel =
    "dev.flutter.pigeon.simple_query_windows.NativeQueryFlutterApi."
    "onObserveEvent";
// What the plugin logs when Dart rejects an observe event.
const std::string kRejectedEventLog = "\"event\":\"observe_event_rejected\"";

// How the fake Dart side answers an observe event.
enum class EventReply {
  // Never answers, like a Dart side that has not replied yet.
  kNone,
  // Answers that the event was handled.
  kAccept,
  // Answers with an error, like a Dart side that stopped listening.
  kReject,
};

// Purpose: Hold one observe event sent through a fake messenger.
struct SentObserveEvent {
  std::string observer_id;
  EncodableMap event;
  std::thread::id thread;
};

// Purpose: Stand in for one engine's messenger, recording every message
// sent and every handler registered, and answering observe events the way
// the test chooses.
class FakeMessenger : public flutter::BinaryMessenger {
 public:
  // Purpose: Record one message and the thread that sent it, then answer an
  // observe event as chosen by SetEventReply.
  // @param channel is the channel name.
  // @param message is the encoded message.
  // @param message_size is the message length in bytes.
  // @param reply receives the answer.
  // @returns Nothing.
  // @throws Nothing beyond what reply throws.
  void Send(const std::string& channel, const uint8_t* message,
            size_t message_size, flutter::BinaryReply reply) const override {
    EventReply event_reply = EventReply::kNone;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sent_.push_back({channel, std::vector<uint8_t>(message, message + message_size),
                       std::this_thread::get_id()});
      event_reply = event_reply_;
    }
    if (channel != kObserveEventChannel || !reply || event_reply == EventReply::kNone) {
      return;
    }
    const EncodableList answer =
        event_reply == EventReply::kAccept
            ? EncodableList{}
            : EncodableList{EncodableValue("listener-gone"),
                            EncodableValue("no listener"), EncodableValue()};
    const auto encoded =
        NativeQueryFlutterApi::GetCodec().EncodeMessage(EncodableValue(answer));
    reply(encoded->data(), encoded->size());
  }

  // Purpose: Record or remove one channel's handler.
  // @param channel is the channel name.
  // @param handler is the new handler; empty removes the channel's handler.
  // @returns Nothing.
  // @throws Nothing.
  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (handler) {
      handlers_[channel] = std::move(handler);
    } else {
      handlers_.erase(channel);
    }
  }

  // Purpose: Choose how later observe events are answered.
  // @param event_reply is the answer to give.
  // @returns Nothing.
  // @throws Nothing.
  void SetEventReply(EventReply event_reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_reply_ = event_reply;
  }

  // Purpose: Report whether a channel has a handler.
  // @param channel is the channel name.
  // @returns true when a handler is registered.
  // @throws Nothing.
  bool HasHandler(const std::string& channel) const {
    return handlers_.find(channel) != handlers_.end();
  }

  // Purpose: Call one host API method the way Dart would.
  // @param method is the Pigeon method name, such as "observeStart".
  // @param arguments are the method's arguments.
  // @returns The decoded reply list: one value on success, or an error's
  // code, message and details. Empty when no handler answered.
  // @throws std::bad_variant_access when the reply is not a list.
  EncodableList CallHost(const std::string& method,
                         const EncodableList& arguments) {
    const auto handler = handlers_.find(kHostChannelPrefix + method);
    if (handler == handlers_.end()) {
      ADD_FAILURE() << "no handler for " << method;
      return {};
    }
    const auto& codec = NativeQueryHostApi::GetCodec();
    const auto message = codec.EncodeMessage(EncodableValue(arguments));
    EncodableList reply_list;
    handler->second(message->data(), message->size(),
                    [&codec, &reply_list](const uint8_t* reply, size_t reply_size) {
                      reply_list = std::get<EncodableList>(
                          *codec.DecodeMessage(reply, reply_size));
                    });
    return reply_list;
  }

  // Purpose: Decode every observe event sent so far.
  // @returns The events, in the order they were sent.
  // @throws std::bad_variant_access when an event is malformed.
  std::vector<SentObserveEvent> ObserveEvents() const {
    const auto& codec = NativeQueryFlutterApi::GetCodec();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SentObserveEvent> events;
    for (const auto& sent : sent_) {
      if (sent.channel != kObserveEventChannel) {
        continue;
      }
      const auto decoded = codec.DecodeMessage(sent.bytes.data(), sent.bytes.size());
      const auto& arguments = std::get<EncodableList>(*decoded);
      events.push_back({std::get<std::string>(arguments.at(0)),
                        std::get<EncodableMap>(arguments.at(1)), sent.thread});
    }
    return events;
  }

 private:
  // Purpose: Hold one sent message.
  struct SentMessage {
    std::string channel;
    std::vector<uint8_t> bytes;
    std::thread::id thread;
  };

  // Send is const in the messenger interface, so its record is mutable.
  mutable std::mutex mutex_;
  mutable std::vector<SentMessage> sent_;
  EventReply event_reply_ = EventReply::kNone;
  std::map<std::string, flutter::BinaryMessageHandler> handlers_;
};

// Purpose: Describe a files observer to start through the host API.
struct FileObserverRequest {
  // The engine's messenger.
  FakeMessenger& messenger;
  // The directory to watch.
  std::filesystem::path root;
  // The requested polling interval.
  int64_t polling_interval_ms = 0;
};

// Purpose: Start a files observer through the host API, as Dart would.
// @param request names the messenger, the directory and the interval.
// @returns The observer id, or empty when observeStart failed.
// @throws std::bad_variant_access when the reply is malformed.
std::string StartFileObserver(const FileObserverRequest& request) {
  const EncodableMap platform_data{
      {EncodableValue("rootPath"), EncodableValue(request.root.string())}};
  const EncodableMap observe_request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue(request.polling_interval_ms)},
      {EncodableValue("platformData"), EncodableValue(platform_data)}};
  const EncodableList reply = request.messenger.CallHost(
      "observeStart", EncodableList{EncodableValue(observe_request)});
  if (reply.size() != 1) {
    ADD_FAILURE() << "observeStart did not return an observer id";
    return "";
  }
  return std::get<std::string>(reply.at(0));
}

// Purpose: Write new files until the messenger receives an observe event.
// The worker's first snapshot runs at an unknown moment, and a file written
// before it is part of its baseline, so one write might never be reported.
// @param messenger is the engine's messenger.
// @param directory is the watched directory.
// @returns true when an event arrived before the delivery timeout.
// @throws Nothing.
bool WriteFilesUntilObserveEvent(const FakeMessenger& messenger,
                                 const TempDirectory& directory) {
  const size_t before = messenger.ObserveEvents().size();
  const auto deadline = std::chrono::steady_clock::now() + kDeliveryTimeout;
  int written = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    directory.WriteFile("change_" + std::to_string(written++) + ".txt");
    if (PumpUntil([&]() { return messenger.ObserveEvents().size() > before; },
                  2 * std::chrono::milliseconds(kPluginFastPollMs))) {
      return true;
    }
  }
  return false;
}

// Purpose: Count the captured log lines that report a rejected event.
// @param log is the captured standard error.
// @returns How many such lines there are.
// @throws Nothing.
size_t RejectedEventLines(const StandardErrorCapture& log) {
  const auto lines = log.Lines();
  return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [](const auto& line) {
    return line.find(kRejectedEventLog) != std::string::npos;
  }));
}

// Purpose: The plugin sends file observe events on the platform thread.
TEST(SimpleQueryWindowsPluginTest, SendsObserveEventsOnThePlatformThread) {
  FakeMessenger messenger;
  TempDirectory directory;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const std::string observer_id =
      StartFileObserver({.messenger = messenger,
                         .root = directory.path(),
                         .polling_interval_ms = kPluginFastPollMs});
  ASSERT_FALSE(observer_id.empty());

  ASSERT_TRUE(WriteFilesUntilObserveEvent(messenger, directory));

  for (const auto& sent : messenger.ObserveEvents()) {
    EXPECT_EQ(sent.observer_id, observer_id);
    EXPECT_EQ(sent.thread, std::this_thread::get_id());
    ASSERT_FALSE(IdsOf(sent.event).empty());
    for (const auto& id : IdsOf(sent.event)) {
      EXPECT_EQ(std::filesystem::path(id).parent_path(), directory.path());
    }
  }
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: Each engine's plugin answers and reports on its own messenger,
// and removing one engine leaves the other working.
TEST(SimpleQueryWindowsPluginTest, KeepsEachEnginesObserversSeparate) {
  FakeMessenger first_messenger;
  FakeMessenger second_messenger;
  TempDirectory first_directory;
  TempDirectory second_directory;
  auto first_plugin = std::make_unique<SimpleQueryWindowsPlugin>(&first_messenger);
  SimpleQueryWindowsPlugin second_plugin(&second_messenger);
  ASSERT_FALSE(StartFileObserver({.messenger = first_messenger,
                                  .root = first_directory.path(),
                                  .polling_interval_ms = kPluginFastPollMs})
                   .empty());
  ASSERT_FALSE(StartFileObserver({.messenger = second_messenger,
                                  .root = second_directory.path(),
                                  .polling_interval_ms = kPluginFastPollMs})
                   .empty());

  ASSERT_TRUE(WriteFilesUntilObserveEvent(first_messenger, first_directory));
  EXPECT_TRUE(second_messenger.ObserveEvents().empty());

  first_plugin.reset();
  EXPECT_FALSE(first_messenger.HasHandler(kObserveStartChannel));
  EXPECT_TRUE(second_messenger.HasHandler(kObserveStartChannel));

  const size_t first_events = first_messenger.ObserveEvents().size();
  ASSERT_TRUE(WriteFilesUntilObserveEvent(second_messenger, second_directory));
  EXPECT_EQ(first_messenger.ObserveEvents().size(), first_events);
}

// Purpose: Destroying the plugin does not wait out a long polling interval.
TEST(SimpleQueryWindowsPluginTest, TearsDownPromptlyWithALongPollingInterval) {
  FakeMessenger messenger;
  TempDirectory directory;
  auto plugin = std::make_unique<SimpleQueryWindowsPlugin>(&messenger);
  ASSERT_FALSE(StartFileObserver({.messenger = messenger,
                                  .root = directory.path(),
                                  .polling_interval_ms = kPluginLongPollMs})
                   .empty());

  EXPECT_LT(TimeOf([&]() { plugin.reset(); }), kPromptTeardown);
  EXPECT_FALSE(messenger.HasHandler(kObserveStartChannel));
}

// Purpose: A run of rejected events is logged once, and an accepted event
// ends the run, so the next rejection is logged again.
TEST(SimpleQueryWindowsPluginTest, LogsEachRunOfRejectedEventsOnce) {
  // Declared first, so it outlives every thread the plugin starts.
  StandardErrorCapture log;
  {
    FakeMessenger messenger;
    TempDirectory directory;
    SimpleQueryWindowsPlugin plugin(&messenger);
    ASSERT_FALSE(StartFileObserver({.messenger = messenger,
                                    .root = directory.path(),
                                    .polling_interval_ms = kPluginFastPollMs})
                     .empty());

    messenger.SetEventReply(EventReply::kReject);
    ASSERT_TRUE(WriteFilesUntilObserveEvent(messenger, directory));
    ASSERT_TRUE(WriteFilesUntilObserveEvent(messenger, directory));
    messenger.SetEventReply(EventReply::kAccept);
    ASSERT_TRUE(WriteFilesUntilObserveEvent(messenger, directory));
    messenger.SetEventReply(EventReply::kReject);
    ASSERT_TRUE(WriteFilesUntilObserveEvent(messenger, directory));
    // Leaving the scope joins the plugin's workers before the log is read.
  }

  EXPECT_EQ(RejectedEventLines(log), 2u);
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
