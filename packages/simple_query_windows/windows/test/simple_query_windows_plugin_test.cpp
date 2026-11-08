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
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "boundary_formats.h"
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
  // Answers that the event was handled.
  kAccept,
  // Answers with an error, like a Dart side that stopped listening.
  kReject,
};

// Purpose: Hold one observe event sent through a fake messenger.
// @param observer_id is the id the event was sent for.
// @param event is decoded from the sent message.
// @param thread lets a test check that events leave on the platform
// thread.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct SentObserveEvent {
  std::string observer_id;
  EncodableMap event;
  std::thread::id thread;
};

// Purpose: Stand in for one engine's messenger, recording every message
// sent and every handler registered, and answering observe events the way
// the test chooses.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class FakeMessenger : public flutter::BinaryMessenger {
 public:
  // Purpose: Record one message and the thread that sent it, then answer an
  // observe event as chosen by SetEventReply. The plugin sends an
  // observer's next event only after this answer.
  // @param channel is the channel name.
  // @param message is the encoded message.
  // @param message_size is the message length in bytes.
  // @param reply receives the answer.
  // @returns Nothing.
  // @throws Nothing beyond what reply throws.
  // flutter::BinaryMessenger fixes this signature.
  void Send(const std::string& channel, const uint8_t* message,
            size_t message_size, flutter::BinaryReply reply) const override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sent_.push_back({channel, std::vector<uint8_t>(message, message + message_size),
                       std::this_thread::get_id()});
    }
    if (channel != kObserveEventChannel || !reply) {
      return;
    }
    const EventReply event_reply = GetEventReply();
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

  // Purpose: Read how observe events are being answered.
  // @returns The answer SetEventReply chose; kAccept until it is called.
  // @throws Nothing.
  EventReply GetEventReply() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return event_reply_;
  }

  // Purpose: Report whether a channel has a handler.
  // @param channel is the channel name.
  // @returns true when a handler is registered.
  // @throws Nothing.
  bool HasHandler(const std::string& channel) const {
    return handlers_.find(channel) != handlers_.end();
  }

  // Purpose: Call one host API method the way Dart would, pumping this
  // thread's messages until the reply arrives, since slow calls reply from
  // the platform thread after their background work.
  // @param method is the Pigeon method name, such as "observeStart".
  // @param arguments are the method's arguments.
  // @returns The decoded reply list: one value on success, or an error's
  // code, message and details. Empty when no handler answered in time.
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
    // Shared, so a reply arriving after a timed-out call writes to live
    // memory.
    auto reply_list = std::make_shared<std::optional<EncodableList>>();
    handler->second(message->data(), message->size(),
                    [&codec, reply_list](const uint8_t* reply, size_t reply_size) {
                      *reply_list = std::get<EncodableList>(
                          *codec.DecodeMessage(reply, reply_size));
                    });
    if (!PumpUntil([&reply_list]() { return reply_list->has_value(); }, kDeliveryTimeout)) {
      ADD_FAILURE() << method << " did not reply";
      return {};
    }
    return **reply_list;
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
  // @param channel is the channel it was sent on.
  // @param bytes are the encoded message.
  // @param thread lets a test check that events leave on the platform
  // thread.
  // @returns Nothing; this is a data type.
  // @throws Nothing.
  struct SentMessage {
    std::string channel;
    std::vector<uint8_t> bytes;
    std::thread::id thread;
  };

  // Send is const in the messenger interface, so its record is mutable.
  mutable std::mutex mutex_;
  mutable std::vector<SentMessage> sent_;
  // Guarded by mutex_.
  EventReply event_reply_ = EventReply::kAccept;
  std::map<std::string, flutter::BinaryMessageHandler> handlers_;
};

// Purpose: Describe a files observer to start through the host API.
// @param messenger receives the observeStart call; it must outlive the
// request.
// @param root is sent as the request's rootPath.
// @param polling_interval_ms is sent as pollingIntervalMs; the plugin
// raises it to its floor.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileObserverRequest {
  FakeMessenger& messenger;
  std::filesystem::path root;
  int64_t polling_interval_ms = 0;
};

// Purpose: Start a files observer through the host API, as Dart would.
// @param request names the messenger, the directory and the interval.
// @returns The observer id, or empty when observeStart failed.
// @throws std::bad_variant_access when the reply is malformed.
std::string StartFileObserver(const FileObserverRequest& request) {
  const EncodableMap platform_data{
      {EncodableValue("rootPath"), EncodableValue(PathToUtf8(request.root))}};
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

// Purpose: Write new files until the messenger receives another observe
// event. A write that lands while the previous event waits for its answer
// is folded into the next event, so the test writes again rather than
// counting writes.
// @param messenger receives the observeStart call; it must outlive the
// request.
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
  return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
    return line.find(kRejectedEventLog) != std::string::npos;
  }));
}

// Purpose: The plugin sends file observe events on the platform thread.
TEST(SimpleQueryWindowsPluginTest, SendsObserveEventsOnThePlatformThread) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
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
      EXPECT_EQ(PathFromUtf8(id).parent_path(), directory.path());
    }
    const auto& timestamp = std::get<std::string>(sent.event.at(EncodableValue("timestamp")));
    EXPECT_TRUE(IsIsoUtc(timestamp)) << timestamp;
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
  DetachedThreadsDrain drain;
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
  DetachedThreadsDrain drain;
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
    DetachedThreadsDrain drain;
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
    // Leaving the scope waits for the plugin's workers before the log is
    // read, and a stopping worker logs nothing.
  }

  EXPECT_EQ(RejectedEventLines(log), 2u);
}

// Purpose: A working directory that cannot be read fails observeStart with
// unavailable and starts no observer.
TEST(SimpleQueryWindowsPluginTest, ReportsAnUnreadableRootWithoutStartingAnObserver) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(
      &messenger,
      {.current_directory = []() -> std::filesystem::path {
        throw std::system_error(
            std::make_error_code(std::errc::no_such_file_or_directory),
            "working directory unavailable");
      }});
  // No rootPath, so the root is the working directory.
  const EncodableMap observe_request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue(kPluginLongPollMs)}};

  const EncodableList reply = messenger.CallHost(
      "observeStart", EncodableList{EncodableValue(observe_request)});

  ASSERT_EQ(reply.size(), 3u);
  EXPECT_EQ(std::get<std::string>(reply.at(0)), "unavailable");
  // Each Start call takes the next id, so getting the first id shows the
  // failed call never reached Start.
  const std::string observer_id =
      StartFileObserver({.messenger = messenger,
                         .root = directory.path(),
                         .polling_interval_ms = kPluginLongPollMs});
  EXPECT_EQ(observer_id, "windows_observer_1");
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: An observe root whose records cannot be read fails observeStart
// before it replies, with unavailable, and starts no observer.
TEST(SimpleQueryWindowsPluginTest, ReportsAnUnreadableBaselineWithoutStartingAnObserver) {
  FakeMessenger messenger;
  TempDirectory directory;
  const std::filesystem::path not_a_folder = directory.WriteFile("not_a_folder.txt");
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap observe_request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue(kPluginLongPollMs)},
      {EncodableValue("platformData"),
       EncodableValue(EncodableMap{
           {EncodableValue("rootPath"), EncodableValue(PathToUtf8(not_a_folder))}})}};

  const EncodableList reply = messenger.CallHost(
      "observeStart", EncodableList{EncodableValue(observe_request)});

  ASSERT_EQ(reply.size(), 3u);
  EXPECT_EQ(std::get<std::string>(reply.at(0)), "unavailable");
  EXPECT_NE(std::get<std::string>(reply.at(1)).find("start again"), std::string::npos);
  // Each Start call takes the next id, so getting the first id shows the
  // failed call never reached Start.
  const std::string observer_id =
      StartFileObserver({.messenger = messenger,
                         .root = directory.path(),
                         .polling_interval_ms = kPluginLongPollMs});
  EXPECT_EQ(observer_id, "windows_observer_1");
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: observeStart takes the default interval when Dart sends a null
// pollingIntervalMs, which it does whenever the caller sets no interval.
TEST(SimpleQueryWindowsPluginTest, StartsAnObserverWhenTheIntervalIsNull) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap observe_request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue()},
      {EncodableValue("platformData"),
       EncodableValue(EncodableMap{
           {EncodableValue("rootPath"), EncodableValue(PathToUtf8(directory.path()))}})}};

  const EncodableList reply = messenger.CallHost(
      "observeStart", EncodableList{EncodableValue(observe_request)});

  ASSERT_EQ(reply.size(), 1u);
  const std::string observer_id = std::get<std::string>(reply.at(0));
  EXPECT_EQ(observer_id, "windows_observer_1");
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: observeStart rejects a pollingIntervalMs that is not a number,
// rather than guessing an interval, and starts no observer.
TEST(SimpleQueryWindowsPluginTest, RejectsAnIntervalThatIsNotANumber) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap observe_request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue("soon")},
      {EncodableValue("platformData"),
       EncodableValue(EncodableMap{
           {EncodableValue("rootPath"), EncodableValue(PathToUtf8(directory.path()))}})}};

  const EncodableList reply = messenger.CallHost(
      "observeStart", EncodableList{EncodableValue(observe_request)});

  ASSERT_EQ(reply.size(), 3u);
  EXPECT_EQ(std::get<std::string>(reply.at(0)), "invalid-query");
}

// Purpose: A change made right after observeStart replies is reported,
// because the baseline is taken before the reply.
TEST(SimpleQueryWindowsPluginTest, ReportsAChangeMadeRightAfterTheStartReply) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const std::string observer_id =
      StartFileObserver({.messenger = messenger,
                         .root = directory.path(),
                         .polling_interval_ms = kPluginFastPollMs});
  ASSERT_FALSE(observer_id.empty());

  const std::filesystem::path added = directory.WriteFile("added.txt");

  ASSERT_TRUE(PumpUntil([&]() { return !messenger.ObserveEvents().empty(); },
                        kDeliveryTimeout));
  const auto events = messenger.ObserveEvents();
  EXPECT_EQ(IdsOf(events[0].event), (std::vector<std::string>{PathToUtf8(added)}));
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: Read the error code of a failed host call.
// @param reply is the decoded reply list.
// @returns The code, or empty text when the call succeeded.
// @throws std::bad_variant_access when the code is not text.
std::string ErrorCodeOf(const EncodableList& reply) {
  return reply.size() == 3 ? std::get<std::string>(reply.at(0)) : std::string();
}

// Purpose: Build a files query rooted at a folder.
// @param root is the folder.
// @returns The request, without filters or paging.
// @throws std::system_error when the root is not valid Unicode.
EncodableMap FilesQuery(const std::filesystem::path& root) {
  return {{EncodableValue("domain"), EncodableValue("files")},
          {EncodableValue("platformData"),
           EncodableValue(EncodableMap{
               {EncodableValue("rootPath"), EncodableValue(PathToUtf8(root))}})}};
}

// Purpose: A query without a domain is rejected as invalid-query.
TEST(SimpleQueryWindowsPluginTest, RejectsAQueryWithoutADomain) {
  FakeMessenger messenger;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);

  const EncodableList reply =
      messenger.CallHost("query", EncodableList{EncodableValue(EncodableMap{})});

  EXPECT_EQ(ErrorCodeOf(reply), "invalid-query");
}

// Purpose: A query filter with an operator Dart does not define is rejected
// as invalid-query instead of matching everything.
TEST(SimpleQueryWindowsPluginTest, RejectsAQueryWithAnUnknownOperator) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  EncodableMap request = FilesQuery(directory.path());
  request[EncodableValue("filters")] = EncodableValue(EncodableList{EncodableValue(
      EncodableMap{{EncodableValue("field"), EncodableValue("name")},
                   {EncodableValue("operator"), EncodableValue("startsWith")},
                   {EncodableValue("value"), EncodableValue("a")}})});

  const EncodableList reply = messenger.CallHost("query", EncodableList{EncodableValue(request)});

  EXPECT_EQ(ErrorCodeOf(reply), "invalid-query");
}

// Purpose: A filtered query applies its filter on the background worker,
// after the decoded request has gone, and keeps only the matching file.
TEST(SimpleQueryWindowsPluginTest, FiltersAQueryOnTheBackgroundWorker) {
  FakeMessenger messenger;
  TempDirectory directory;
  directory.WriteFile("kept.txt");
  directory.WriteFile("dropped.txt");
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  EncodableMap request = FilesQuery(directory.path());
  request[EncodableValue("filters")] = EncodableValue(EncodableList{EncodableValue(
      EncodableMap{{EncodableValue("field"), EncodableValue("name")},
                   {EncodableValue("operator"), EncodableValue("equals")},
                   {EncodableValue("value"), EncodableValue("kept.txt")}})});

  const EncodableList reply = messenger.CallHost("query", EncodableList{EncodableValue(request)});

  ASSERT_EQ(reply.size(), 1u) << ErrorCodeOf(reply);
  const auto& result = std::get<EncodableMap>(reply.at(0));
  const auto& records = std::get<EncodableList>(result.at(EncodableValue("records")));
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(std::get<std::string>(std::get<EncodableMap>(records.at(0)).at(EncodableValue("name"))),
            "kept.txt");
}

// Purpose: A files query reads a UTF-8 root, returns UTF-8 paths, and pages
// with the total count and the next offset.
TEST(SimpleQueryWindowsPluginTest, QueriesAUtf8RootAPageAtATime) {
  FakeMessenger messenger;
  TempDirectory directory;
  const std::filesystem::path root = directory.path() / PathFromUtf8("caf\xC3\xA9");
  std::filesystem::create_directories(root);
  for (int file = 0; file < 3; ++file) {
    std::ofstream(root / ("file_" + std::to_string(file) + ".txt")) << "changed";
  }
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  EncodableMap request = FilesQuery(root);
  request[EncodableValue("page")] = EncodableValue(
      EncodableMap{{EncodableValue("limit"), EncodableValue(int32_t{2})}});

  const EncodableList reply = messenger.CallHost("query", EncodableList{EncodableValue(request)});

  ASSERT_EQ(reply.size(), 1u) << ErrorCodeOf(reply);
  const auto& result = std::get<EncodableMap>(reply.at(0));
  const auto& records = std::get<EncodableList>(result.at(EncodableValue("records")));
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(std::get<int64_t>(result.at(EncodableValue("totalCount"))), 3);
  EXPECT_EQ(std::get<int64_t>(result.at(EncodableValue("nextOffset"))), 2);
  for (const auto& record : records) {
    const auto& path =
        std::get<std::string>(std::get<EncodableMap>(record).at(EncodableValue("path")));
    EXPECT_EQ(PathFromUtf8(path).parent_path(), root);
  }
}

// Purpose: openBinary reads a UTF-8 path and returns it unchanged as the
// local path.
TEST(SimpleQueryWindowsPluginTest, OpensABinaryByItsUtf8Path) {
  FakeMessenger messenger;
  TempDirectory directory;
  const std::string utf8_name = "\xE6\x97\xA5\xE8\xA8\x98.txt";
  const std::filesystem::path file = directory.path() / PathFromUtf8(utf8_name);
  std::ofstream(file) << "changed";
  const std::string file_text = PathToUtf8(file);
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("platformData"),
       EncodableValue(EncodableMap{{EncodableValue("path"), EncodableValue(file_text)}})}};

  const EncodableList reply =
      messenger.CallHost("openBinary", EncodableList{EncodableValue(request)});

  ASSERT_EQ(reply.size(), 1u) << ErrorCodeOf(reply);
  const auto& result = std::get<EncodableMap>(reply.at(0));
  EXPECT_EQ(std::get<std::string>(result.at(EncodableValue("localPath"))), file_text);
  EXPECT_EQ(std::get<int64_t>(result.at(EncodableValue("size"))), 7);
  messenger.CallHost("closeBinary",
                     EncodableList{result.at(EncodableValue("handleId"))});
}

// Purpose: openBinary for a missing file fails as unavailable and says what
// to do next.
TEST(SimpleQueryWindowsPluginTest, ReportsAMissingBinaryAsUnavailable) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("platformData"),
       EncodableValue(EncodableMap{
           {EncodableValue("path"),
            EncodableValue(PathToUtf8(directory.path() / "missing.txt"))}})}};

  const EncodableList reply =
      messenger.CallHost("openBinary", EncodableList{EncodableValue(request)});

  ASSERT_EQ(ErrorCodeOf(reply), "unavailable");
  EXPECT_NE(std::get<std::string>(reply.at(1)).find("Query again"), std::string::npos);
}

// Purpose: Relative mutation roots are captured on the platform thread,
// keeping queued work tied to the caller's original directory.
TEST(SimpleQueryWindowsPluginTest, CapturesRelativeMutationRootsBeforeQueueing) {
  FakeMessenger messenger;
  TempDirectory original;
  TempDirectory later;
  const auto original_root = original.path() / "scope";
  const auto later_root = later.path() / "scope";
  std::filesystem::create_directories(original_root);
  std::filesystem::create_directories(later_root);
  const auto original_file = original_root / "target.txt";
  const auto later_file = later_root / "target.txt";
  std::ofstream(original_file) << "original";
  std::ofstream(later_file) << "later";
  int directory_reads = 0;
  std::thread::id resolution_thread;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger, {
      .current_directory = [&]() {
        resolution_thread = std::this_thread::get_id();
        return ++directory_reads == 1 ? original.path() : later.path();
      }});
  const EncodableMap request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("platformData"), EncodableValue(EncodableMap{
          {EncodableValue("rootPath"), EncodableValue("scope")}})},
      {EncodableValue("filters"), EncodableValue(EncodableList{EncodableValue(EncodableMap{
          {EncodableValue("field"), EncodableValue("name")},
          {EncodableValue("operator"), EncodableValue("equals")},
          {EncodableValue("value"), EncodableValue("target.txt")}})})},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("content"), EncodableValue("replaced")}})}};

  const auto reply = messenger.CallHost("mutate", EncodableList{EncodableValue(request)});
  ASSERT_EQ(reply.size(), 1u);
  EXPECT_EQ(resolution_thread, std::this_thread::get_id());
  EXPECT_EQ(directory_reads, 1);
  std::string original_text;
  std::string later_text;
  std::ifstream(original_file) >> original_text;
  std::ifstream(later_file) >> later_text;
  EXPECT_EQ(original_text, "replaced");
  EXPECT_EQ(later_text, "later");
}

// Purpose: Observer filters cover the baseline and later polls, so unrelated
// files never appear in a filtered subscription's event ids.
TEST(SimpleQueryWindowsPluginTest, ObservesOnlyRecordsMatchingItsFilters) {
  FakeMessenger messenger;
  TempDirectory directory;
  const auto watched = directory.WriteFile("watched.txt");
  directory.WriteFile("existing-other.txt");
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const EncodableMap request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue(kPluginFastPollMs)},
      {EncodableValue("platformData"), EncodableValue(EncodableMap{
          {EncodableValue("rootPath"), EncodableValue(PathToUtf8(directory.path()))}})},
      {EncodableValue("filters"), EncodableValue(EncodableList{EncodableValue(EncodableMap{
          {EncodableValue("field"), EncodableValue("name")},
          {EncodableValue("operator"), EncodableValue("equals")},
          {EncodableValue("value"), EncodableValue("watched.txt")}})})}};
  const auto reply = messenger.CallHost("observeStart", EncodableList{EncodableValue(request)});
  ASSERT_EQ(reply.size(), 1u);
  const auto observer_id = std::get<std::string>(reply[0]);
  directory.WriteFile("new-other.txt");
  std::ofstream(watched, std::ios::app) << "larger";

  ASSERT_TRUE(PumpUntil([&]() { return !messenger.ObserveEvents().empty(); }, kDeliveryTimeout));
  const auto events = messenger.ObserveEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(IdsOf(events[0].event), (std::vector<std::string>{PathToUtf8(watched)}));
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

// Purpose: Invalid observer filters fail before starting any observer.
TEST(SimpleQueryWindowsPluginTest, RejectsMalformedObserverFilters) {
  FakeMessenger messenger;
  TempDirectory directory;
  DetachedThreadsDrain drain;
  SimpleQueryWindowsPlugin plugin(&messenger);
  EncodableMap request = FilesQuery(directory.path());
  request[EncodableValue("filters")] = EncodableValue("not a list");

  const auto reply = messenger.CallHost("observeStart", EncodableList{EncodableValue(request)});

  EXPECT_EQ(ErrorCodeOf(reply), "invalid-query");
  const auto observer_id = StartFileObserver({.messenger = messenger,
      .root = directory.path(), .polling_interval_ms = kPluginLongPollMs});
  EXPECT_EQ(observer_id, "windows_observer_1");
  messenger.CallHost("observeStop", EncodableList{EncodableValue(observer_id)});
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
