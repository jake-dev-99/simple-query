// Native tests for observe delivery: the platform-thread dispatcher, the
// polling observers, file listing and record fingerprints, and the plugin as
// one engine sees it through a fake messenger. Each test runs on the test runner's main thread, which plays
// the platform thread and pumps its own message queue.

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "file_records.h"
#include "include/simple_query_windows/simple_query_windows_plugin.h"
#include "native_query.g.h"
#include "platform_thread_dispatcher.h"
#include "polling_observers.h"

namespace simple_query_windows {
namespace test {
namespace {

using namespace std::chrono_literals;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Long enough for a busy CI machine, short enough to fail fast.
constexpr std::chrono::milliseconds kDeliveryTimeout = 5s;
// Teardown must finish well inside one long polling interval.
constexpr std::chrono::milliseconds kPromptTeardown = 2s;
constexpr std::chrono::milliseconds kFastPoll = 20ms;
constexpr std::chrono::milliseconds kLongPoll = 10min;
// The plugin's smallest polling interval.
constexpr int64_t kPluginFastPollMs = 250;
constexpr int64_t kPluginLongPollMs = 600000;
constexpr int64_t kFixedNowEpochMs = 1700000000000;

constexpr const wchar_t* kDispatcherWindowClass =
    L"SimpleQueryWindowsPlatformThreadDispatcher";

const std::string kHostChannelPrefix =
    "dev.flutter.pigeon.simple_query_windows.NativeQueryHostApi.";
const std::string kObserveStartChannel = kHostChannelPrefix + "observeStart";
const std::string kObserveEventChannel =
    "dev.flutter.pigeon.simple_query_windows.NativeQueryFlutterApi."
    "onObserveEvent";

// Purpose: Run every message already waiting in this thread's queue.
// @returns Nothing.
// @throws Nothing.
void PumpQueuedMessages() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

// Purpose: Pump this thread's messages until a condition holds.
// @param done is the condition to wait for.
// @param timeout is how long to keep pumping.
// @returns true when done held before the timeout.
// @throws Nothing beyond what done throws.
bool PumpUntil(const std::function<bool()>& done,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    PumpQueuedMessages();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Purpose: Wait for a condition without running queued messages, so queued
// work stays queued.
// @param done is the condition to wait for.
// @param timeout is how long to wait.
// @returns true when done held before the timeout.
// @throws Nothing beyond what done throws.
bool WaitWithoutPumping(const std::function<bool()>& done,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Purpose: Post to this thread until Windows refuses more posted messages,
// so the next post to this thread's windows fails.
// @returns true when a post was refused.
// @throws Nothing.
bool FillMessageQueue() {
  // Windows allows 10,000 posted messages per queue by default.
  constexpr int kMaxPosts = 100000;
  for (int post = 0; post < kMaxPosts; ++post) {
    if (!PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0)) {
      return true;
    }
  }
  return false;
}

// Purpose: Measure how long an action takes.
// @param action is the action to time.
// @returns The elapsed time.
// @throws Nothing beyond what action throws.
std::chrono::steady_clock::duration TimeOf(const std::function<void()>& action) {
  const auto started = std::chrono::steady_clock::now();
  action();
  return std::chrono::steady_clock::now() - started;
}

// Purpose: Read the ids an observe event reports.
// @param event is the event map.
// @returns The ids, in the event's order.
// @throws std::bad_variant_access when the event has no id list.
std::vector<std::string> IdsOf(const EncodableMap& event) {
  std::vector<std::string> ids;
  for (const auto& id : std::get<EncodableList>(event.at(EncodableValue("ids")))) {
    ids.push_back(std::get<std::string>(id));
  }
  return ids;
}

// Purpose: Serve snapshots that the test sets, and count how many were taken.
class ControlledSource {
 public:
  // Purpose: Replace the snapshot returned from now on.
  // @param snapshot is the new snapshot.
  // @returns Nothing.
  // @throws Nothing.
  void Set(Snapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snapshot);
  }

  // Purpose: Make every snapshot from now on fail, or succeed again.
  // @param failing is true to make snapshots throw.
  // @returns Nothing.
  // @throws Nothing.
  void SetFailing(bool failing) { failing_.store(failing); }

  // Purpose: Report how many snapshots were taken.
  // @returns The number of snapshots taken so far.
  // @throws Nothing.
  int Taken() const { return taken_.load(); }

  // Purpose: Wait, without pumping, until a poll has fully handled the
  // current snapshot. Two more snapshots must start: the first reads the
  // current snapshot, and the second starts only after the first poll has
  // recorded it and asked for a wake.
  // @returns true when that happened before the delivery timeout.
  // @throws Nothing.
  bool WaitForPollToFinish() const {
    const int seen = Taken();
    return WaitWithoutPumping([this, seen]() { return Taken() >= seen + 2; },
                              kDeliveryTimeout);
  }

  // Purpose: Build the observer callback that reads this source.
  // @returns A snapshot callback; it must not outlive this source. It throws
  // std::runtime_error while the source is failing.
  // @throws Nothing.
  std::function<Snapshot(const StopRequested&)> SnapshotCallback() {
    return [this](const StopRequested&) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++taken_;
      if (failing_.load()) {
        throw std::runtime_error("controlled snapshot failure");
      }
      return snapshot_;
    };
  }

 private:
  std::mutex mutex_;
  Snapshot snapshot_;
  std::atomic<int> taken_{0};
  std::atomic<bool> failing_{false};
};

// Purpose: Capture standard error until destroyed. Destroy it only after
// every thread that writes to it has been joined.
class StandardErrorCapture {
 public:
  StandardErrorCapture() : original_(std::cerr.rdbuf(captured_.rdbuf())) {}
  ~StandardErrorCapture() { std::cerr.rdbuf(original_); }
  StandardErrorCapture(const StandardErrorCapture&) = delete;
  StandardErrorCapture& operator=(const StandardErrorCapture&) = delete;

  // Purpose: Split what was written so far into lines.
  // @returns The captured lines, without their line endings.
  // @throws Nothing.
  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    std::istringstream text(captured_.str());
    for (std::string line; std::getline(text, line);) {
      lines.push_back(line);
    }
    return lines;
  }

 private:
  std::ostringstream captured_;
  std::streambuf* original_;
};

// Purpose: Hold one event sent through PollingObservers.
struct SentEvent {
  std::string observer_id;
  EncodableMap event;
  std::thread::id thread;
};

// Purpose: Hold one observe event sent through a fake messenger.
struct SentObserveEvent {
  std::string observer_id;
  EncodableMap event;
  std::thread::id thread;
};

// Purpose: Stand in for one engine's messenger, recording every message
// sent and every handler registered.
class FakeMessenger : public flutter::BinaryMessenger {
 public:
  // Purpose: Record one message and the thread that sent it.
  // @param channel is the channel name.
  // @param message is the encoded message.
  // @param message_size is the message length in bytes.
  // @returns Nothing.
  // @throws Nothing.
  void Send(const std::string& channel, const uint8_t* message,
            size_t message_size, flutter::BinaryReply) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    sent_.push_back({channel, std::vector<uint8_t>(message, message + message_size),
                     std::this_thread::get_id()});
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
  std::map<std::string, flutter::BinaryMessageHandler> handlers_;
};

// Purpose: Own an empty directory for one test, removed afterwards.
class TempDirectory {
 public:
  // Purpose: Create a new empty directory under the system temp directory.
  // @returns Nothing.
  // @throws std::filesystem::filesystem_error when it cannot be created.
  TempDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("simple_query_windows_test_" +
               std::to_string(GetCurrentProcessId()) + "_" +
               std::to_string(next_index_++))) {
    std::filesystem::create_directories(path_);
  }

  // Purpose: Remove the directory and everything in it.
  // @returns Nothing.
  // @throws Nothing; a failed removal is reported as a test failure.
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
      ADD_FAILURE() << "could not remove " << path_.string() << ": "
                    << error.message();
    }
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  // Purpose: Give the directory's path.
  // @returns The directory's path.
  // @throws Nothing.
  const std::filesystem::path& path() const { return path_; }

  // Purpose: Create one small file in the directory.
  // @param name is the file name.
  // @returns The new file's path.
  // @throws Nothing; a failed write shows up as a missing change.
  std::filesystem::path WriteFile(const std::string& name) const {
    std::ofstream(path_ / name) << "changed";
    return path_ / name;
  }

 private:
  static inline int next_index_ = 0;
  std::filesystem::path path_;
};

// Purpose: Start a files observer through the host API, as Dart would.
// @param messenger is the engine's messenger.
// @param root is the directory to watch.
// @param polling_interval_ms is the requested polling interval.
// @returns The observer id, or empty when observeStart failed.
// @throws std::bad_variant_access when the reply is malformed.
std::string StartFileObserver(FakeMessenger& messenger,
                              const std::filesystem::path& root,
                              int64_t polling_interval_ms) {
  const EncodableMap platform_data{
      {EncodableValue("rootPath"), EncodableValue(root.string())}};
  const EncodableMap request{
      {EncodableValue("domain"), EncodableValue("files")},
      {EncodableValue("pollingIntervalMs"), EncodableValue(polling_interval_ms)},
      {EncodableValue("platformData"), EncodableValue(platform_data)}};
  const EncodableList reply =
      messenger.CallHost("observeStart", EncodableList{EncodableValue(request)});
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

// Purpose: Give the test runner's main thread as the platform thread.
class PollingObserversTest : public ::testing::Test {
 protected:
  // Purpose: Build effects that record each event and its sending thread.
  // @returns Effects bound to this fixture.
  // @throws Nothing.
  PollingObserverEffects RecordingEffects() {
    return {.send_event =
                [this](const std::string& observer_id, const EncodableMap& event) {
                  events_.push_back(
                      {observer_id, event, std::this_thread::get_id()});
                },
            .now_epoch_ms = []() { return kFixedNowEpochMs; }};
  }

  // Purpose: Describe an observer that reads a controlled source.
  // @param source is the snapshot source; it must outlive the observer.
  // @param interval is the polling interval.
  // @returns The observer spec.
  // @throws Nothing.
  static ObserverSpec SpecFor(ControlledSource& source,
                              std::chrono::milliseconds interval) {
    return {.domain = "files",
            .polling_interval = interval,
            .take_snapshot = source.SnapshotCallback()};
  }

  // Purpose: Wait until an observer took its first snapshot.
  // @param source is the observer's source.
  // @returns true when it did before the delivery timeout.
  // @throws Nothing.
  static bool WaitForBaseline(const ControlledSource& source) {
    return WaitWithoutPumping([&source]() { return source.Taken() >= 1; },
                              kDeliveryTimeout);
  }

  std::vector<SentEvent> events_;
};

// Purpose: A wake requested from a worker runs on the creating thread.
TEST(PlatformThreadDispatcherTest, RunsTheWakeOnTheCreatingThread) {
  std::thread::id wake_thread;
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() {
    wake_thread = std::this_thread::get_id();
    ++wakes;
  });
  ASSERT_TRUE(dispatcher.IsReady());

  bool requested = false;
  std::thread worker([&]() { requested = dispatcher.RequestWake(); });
  worker.join();

  ASSERT_TRUE(requested);
  ASSERT_TRUE(PumpUntil([&]() { return wakes == 1; }, kDeliveryTimeout));
  EXPECT_EQ(wake_thread, std::this_thread::get_id());
}

// Purpose: Requests made while a wake is queued add no further messages.
TEST(PlatformThreadDispatcherTest, CoalescesRequestsWhileAWakeIsQueued) {
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; });

  EXPECT_TRUE(dispatcher.RequestWake());
  EXPECT_TRUE(dispatcher.RequestWake());
  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 1);

  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 2);
}

// Purpose: A full queue is reported, and the next request queues normally.
TEST(PlatformThreadDispatcherTest, ReportsAFullQueueAndRecovers) {
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; });
  ASSERT_TRUE(FillMessageQueue());

  EXPECT_FALSE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 0);

  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 1);
}

// Purpose: A wake still queued when the dispatcher goes never runs.
TEST(PlatformThreadDispatcherTest, DropsAQueuedWakeWhenDestroyed) {
  int wakes = 0;
  auto dispatcher =
      std::make_unique<PlatformThreadDispatcher>([&]() { ++wakes; });
  ASSERT_TRUE(dispatcher->RequestWake());

  dispatcher.reset();
  PumpQueuedMessages();

  EXPECT_EQ(wakes, 0);
}

// Purpose: Changes seen before the platform thread runs arrive as one event
// on that thread, covering everything since the last delivery.
TEST_F(PollingObserversTest, DeliversAllChangesSinceTheLastEventInOneEvent) {
  ControlledSource source;
  source.Set({{"a", 1}});
  PollingObservers observers(RecordingEffects());
  ASSERT_TRUE(observers.IsReady());
  const std::string observer_id = observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.Set({{"a", 1}, {"b", 1}, {"c", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));
  PumpQueuedMessages();

  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(events_[0].observer_id, observer_id);
  EXPECT_EQ(events_[0].thread, std::this_thread::get_id());
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(std::get<std::string>(events_[0].event.at(EncodableValue("timestamp"))),
            std::to_string(kFixedNowEpochMs));
}

// Purpose: Destroying the last dispatcher removes its window class, and a
// later dispatcher registers it again.
TEST(PlatformThreadDispatcherTest, RemovesItsWindowClassWithTheLastDispatcher) {
  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  {
    PlatformThreadDispatcher first([]() {});
    PlatformThreadDispatcher second([]() {});
    ASSERT_TRUE(first.IsReady());
    ASSERT_TRUE(second.IsReady());
    EXPECT_TRUE(GetClassInfoExW(GetModuleHandleW(nullptr), kDispatcherWindowClass, &info));
  }
  EXPECT_FALSE(GetClassInfoExW(GetModuleHandleW(nullptr), kDispatcherWindowClass, &info));

  PlatformThreadDispatcher again([]() {});
  EXPECT_TRUE(again.IsReady());
}

// Purpose: Destroying one dispatcher leaves another one delivering wakes.
TEST(PlatformThreadDispatcherTest, KeepsDeliveringAfterAnotherIsDestroyed) {
  int wakes = 0;
  PlatformThreadDispatcher survivor([&]() { ++wakes; });
  { PlatformThreadDispatcher destroyed([]() {}); }

  ASSERT_TRUE(survivor.RequestWake());
  EXPECT_TRUE(PumpUntil([&]() { return wakes == 1; }, kDeliveryTimeout));
}

// Purpose: A failed poll reports nothing, so a passing read error never
// reports records as removed and added again, and later changes still arrive.
TEST_F(PollingObserversTest, SkipsAFailedPollAndKeepsReporting) {
  ControlledSource source;
  source.Set({{"a", 1}, {"b", 1}});
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));

  source.SetFailing(true);
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.SetFailing(false);
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();
  EXPECT_TRUE(events_.empty());

  source.Set({{"a", 1}, {"b", 1}, {"c", 1}});
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"c"}));
}

// Purpose: Each run of failed polls is logged once, naming the domain, so a
// lasting failure cannot flood the log.
TEST_F(PollingObserversTest, LogsEachRunOfFailedPollsOnce) {
  StandardErrorCapture log;
  {
    ControlledSource source;
    source.Set({{"a", 1}});
    PollingObservers observers(RecordingEffects());
    observers.Start(SpecFor(source, kFastPoll));
    ASSERT_TRUE(WaitForBaseline(source));

    source.SetFailing(true);
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetFailing(false);
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetFailing(true);
    ASSERT_TRUE(source.WaitForPollToFinish());
    // Leaving the scope joins the worker before the log is read.
  }

  const std::vector<std::string> lines = log.Lines();
  ASSERT_EQ(lines.size(), 2u);
  for (const auto& line : lines) {
    EXPECT_NE(line.find("files"), std::string::npos);
    EXPECT_NE(line.find("controlled snapshot failure"), std::string::npos);
  }
}

// Purpose: Until a first snapshot succeeds there is no baseline, so the
// first success reports nothing and later changes are measured against it.
TEST_F(PollingObserversTest, WaitsForAFirstSnapshotThatSucceeds) {
  ControlledSource source;
  source.Set({{"a", 1}});
  source.SetFailing(true);
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(source.WaitForPollToFinish());

  source.SetFailing(false);
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();
  EXPECT_TRUE(events_.empty());

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b"}));
}

// Purpose: A change undone before delivery is not reported.
TEST_F(PollingObserversTest, ReportsNothingForAChangeUndoneBeforeDelivery) {
  ControlledSource source;
  source.Set({{"a", 1}});
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.Set({{"a", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();

  EXPECT_TRUE(events_.empty());
}

// Purpose: A stopped observer's queued change is never sent.
TEST_F(PollingObserversTest, DropsTheChangeOfAStoppedObserver) {
  ControlledSource source;
  source.Set({{"a", 1}});
  PollingObservers observers(RecordingEffects());
  const std::string observer_id = observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));
  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());

  observers.Stop(observer_id);
  PumpQueuedMessages();

  EXPECT_TRUE(events_.empty());
}

// Purpose: A change whose wake could not be queued is delivered once the
// queue has room again.
TEST_F(PollingObserversTest, RetriesAWakeThatCouldNotBeQueued) {
  ControlledSource source;
  source.Set({{"a", 1}});
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));
  ASSERT_TRUE(FillMessageQueue());

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));

  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b"}));
}

// Purpose: Stopping does not wait out a long polling interval.
TEST_F(PollingObserversTest, StopsPromptlyDuringALongPollingInterval) {
  ControlledSource source;
  PollingObservers observers(RecordingEffects());
  const std::string observer_id = observers.Start(SpecFor(source, kLongPoll));
  ASSERT_TRUE(WaitForBaseline(source));

  EXPECT_LT(TimeOf([&]() { observers.Stop(observer_id); }), kPromptTeardown);
}

// Purpose: Teardown tells a snapshot in progress to stop, instead of waiting
// for a long scan to finish.
TEST_F(PollingObserversTest, TeardownStopsASnapshotInProgress) {
  std::atomic<bool> scanning{false};
  auto observers = std::make_unique<PollingObservers>(RecordingEffects());
  observers->Start({.domain = "files",
                    .polling_interval = kLongPoll,
                    .take_snapshot = [&scanning](const StopRequested& stop_requested) {
                      scanning.store(true);
                      // Stands in for a scan that runs until it is told to stop.
                      const auto give_up = std::chrono::steady_clock::now() + 10s;
                      while (!stop_requested() &&
                             std::chrono::steady_clock::now() < give_up) {
                        std::this_thread::sleep_for(1ms);
                      }
                      return Snapshot{};
                    }});
  ASSERT_TRUE(WaitWithoutPumping([&]() { return scanning.load(); },
                                 kDeliveryTimeout));

  EXPECT_LT(TimeOf([&]() { observers.reset(); }), kPromptTeardown);
}

// Purpose: Teardown with a change queued for delivery sends nothing later.
TEST_F(PollingObserversTest, TeardownDropsAQueuedChange) {
  ControlledSource source;
  source.Set({{"a", 1}});
  auto observers = std::make_unique<PollingObservers>(RecordingEffects());
  observers->Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));
  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());

  observers.reset();
  PumpQueuedMessages();

  EXPECT_TRUE(events_.empty());
}

// Purpose: Changed ids cover changed, removed and added records.
TEST(ChangedIdsTest, ListsChangedRemovedAndAddedIds) {
  const Snapshot before{{"changed", 1}, {"removed", 1}, {"same", 1}};
  const Snapshot after{{"added", 1}, {"changed", 2}, {"same", 1}};

  std::vector<std::string> ids;
  for (const auto& id : ChangedIds(before, after)) {
    ids.push_back(std::get<std::string>(id));
  }

  EXPECT_EQ(ids, (std::vector<std::string>{"added", "changed", "removed"}));
}

// Purpose: Equal rows give equal fingerprints.
TEST(FingerprintRecordsTest, GivesEqualRowsEqualFingerprints) {
  const EncodableMap row{{EncodableValue("id"), EncodableValue("a")},
                         {EncodableValue("title"), EncodableValue("Lunch")}};

  EXPECT_EQ(FingerprintRecords({row}), FingerprintRecords({row}));
}

// Purpose: Changing any field changes the row's fingerprint.
TEST(FingerprintRecordsTest, ChangesWhenAnyFieldChanges) {
  const EncodableMap before{{EncodableValue("id"), EncodableValue("a")},
                            {EncodableValue("title"), EncodableValue("Lunch")}};
  EncodableMap after = before;
  after[EncodableValue("title")] = EncodableValue("Dinner");

  EXPECT_NE(FingerprintRecords({before}).at("a"), FingerprintRecords({after}).at("a"));
}

// Purpose: A row without a string id is left out.
TEST(FingerprintRecordsTest, LeavesOutRowsWithoutAStringId) {
  const EncodableMap no_id{{EncodableValue("title"), EncodableValue("Lunch")}};
  const EncodableMap number_id{{EncodableValue("id"), EncodableValue(7)}};

  EXPECT_TRUE(FingerprintRecords({no_id, number_id}).empty());
}

// Purpose: Rewriting a media file changes its fingerprint, so media
// observers see modifications and not only additions and removals.
TEST(FingerprintRecordsTest, ChangesWhenAMediaFileIsRewritten) {
  TempDirectory directory;
  const auto photo = directory.WriteFile("photo.jpg");
  const auto list_media = [&directory]() {
    return FingerprintRecords(
        ListRecords({.root = directory.path(), .media_only = true}));
  };
  const Snapshot before = list_media();

  std::filesystem::last_write_time(photo,
                                   std::filesystem::last_write_time(photo) + 1h);

  ASSERT_EQ(before.size(), 1u);
  EXPECT_NE(before.at(photo.string()), list_media().at(photo.string()));
}

// Purpose: Without a stop check the walk lists every file.
TEST(ListRecordsTest, ListsEveryFileWithoutAStopCheck) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }

  EXPECT_EQ(ListRecords({.root = directory.path()}).size(), 5u);
}

// Purpose: The walk ends as soon as its stop check turns true.
TEST(ListRecordsTest, StopsWalkingOnceAskedTo) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }
  int checks = 0;
  // Checked once before each entry, so the third check ends the walk after
  // two entries.
  const auto rows = ListRecords(
      {.root = directory.path(), .stop_requested = [&checks]() { return ++checks > 2; }});

  EXPECT_EQ(rows.size(), 2u);
}

// Purpose: A walk asked to stop before it starts lists nothing.
TEST(ListRecordsTest, ListsNothingWhenAlreadyStopped) {
  TempDirectory directory;
  directory.WriteFile("file.txt");

  EXPECT_TRUE(
      ListRecords({.root = directory.path(), .stop_requested = []() { return true; }})
          .empty());
}

// Purpose: The plugin sends file observe events on the platform thread.
TEST(SimpleQueryWindowsPluginTest, SendsObserveEventsOnThePlatformThread) {
  FakeMessenger messenger;
  TempDirectory directory;
  SimpleQueryWindowsPlugin plugin(&messenger);
  const std::string observer_id =
      StartFileObserver(messenger, directory.path(), kPluginFastPollMs);
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
  ASSERT_FALSE(
      StartFileObserver(first_messenger, first_directory.path(), kPluginFastPollMs)
          .empty());
  ASSERT_FALSE(StartFileObserver(second_messenger, second_directory.path(),
                                 kPluginFastPollMs)
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
  ASSERT_FALSE(
      StartFileObserver(messenger, directory.path(), kPluginLongPollMs).empty());

  EXPECT_LT(TimeOf([&]() { plugin.reset(); }), kPromptTeardown);
  EXPECT_FALSE(messenger.HasHandler(kObserveStartChannel));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
