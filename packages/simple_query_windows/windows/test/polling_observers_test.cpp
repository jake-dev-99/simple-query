// Native tests for the polling observers: change detection on worker
// threads, and delivery on the platform thread.

#include "polling_observers.h"

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using namespace std::chrono_literals;
using flutter::EncodableMap;
using flutter::EncodableValue;

constexpr std::chrono::milliseconds kFastPoll = 20ms;
constexpr std::chrono::milliseconds kLongPoll = 10min;
constexpr int64_t kFixedNowEpochMs = 1700000000000;
// kFixedNowEpochMs as the ISO 8601 UTC text Dart's DateTime.parse reads.
constexpr const char* kFixedNowIso = "2023-11-14T22:13:20.000Z";

// Purpose: Say whether a ControlledSource's snapshots fail.
struct ReadOutcome {
  // true makes every snapshot throw.
  bool failing = false;
};

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
  // @param outcome says whether snapshots throw.
  // @returns Nothing.
  // @throws Nothing.
  void SetReadOutcome(ReadOutcome outcome) { failing_.store(outcome.failing); }

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

// Purpose: Hold one event sent through PollingObservers.
struct SentEvent {
  std::string observer_id;
  EncodableMap event;
  std::thread::id thread;
};

// Purpose: Give the test runner's main thread as the platform thread.
class PollingObserversTest : public ::testing::Test {
 protected:
  // Purpose: Build effects that record each event and its sending thread,
  // and answer it at once unless the test holds replies.
  // @returns Effects bound to this fixture.
  // @throws Nothing.
  PollingObserverEffects RecordingEffects() {
    return {.send_event =
                [this](const std::string& observer_id, const EncodableMap& event,
                       std::function<void()> on_settled) {
                  events_.push_back(
                      {observer_id, event, std::this_thread::get_id()});
                  if (hold_replies_) {
                    held_replies_.push_back(std::move(on_settled));
                  } else {
                    on_settled();
                  }
                },
            .now_epoch_ms = []() { return kFixedNowEpochMs; }};
  }

  // Purpose: Describe an observer that reads a controlled source.
  // @param source is the snapshot source; it must outlive the observer's
  // worker, so declare a DetachedThreadsDrain after it.
  // @param interval is the polling interval.
  // @returns The observer spec.
  // @throws Nothing.
  static ObserverSpec SpecFor(ControlledSource& source,
                              std::chrono::milliseconds interval) {
    return {.domain = "files",
            .polling_interval = interval,
            .take_snapshot = source.SnapshotCallback()};
  }

  std::vector<SentEvent> events_;
  // When true, events are recorded but Dart's reply is held back.
  bool hold_replies_ = false;
  // The replies held back, in the order their events were sent.
  std::vector<std::function<void()>> held_replies_;
};

// Purpose: Changes seen before the platform thread runs arrive as one event
// on that thread, covering everything since the last delivery.
TEST_F(PollingObserversTest, DeliversAllChangesSinceTheLastEventInOneEvent) {
  ControlledSource source;
  source.Set({{"a", 1}});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  ASSERT_TRUE(observers.IsReady());
  const std::string observer_id =
      observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});

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
            kFixedNowIso);
}

// Purpose: A failed poll reports nothing, so a passing read error never
// reports records as removed and added again, and later changes still arrive.
TEST_F(PollingObserversTest, SkipsAFailedPollAndKeepsReporting) {
  ControlledSource source;
  source.Set({{"a", 1}, {"b", 1}});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}, {"b", 1}});

  source.SetReadOutcome({.failing = true});
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.SetReadOutcome({.failing = false});
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
    DetachedThreadsDrain drain;
    PollingObservers observers(RecordingEffects());
    observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});

    source.SetReadOutcome({.failing = true});
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetReadOutcome({.failing = false});
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetReadOutcome({.failing = true});
    ASSERT_TRUE(source.WaitForPollToFinish());
    // Leaving the scope waits for the worker before the log is read, and a
    // worker that is stopping logs nothing.
  }

  const std::vector<std::string> lines = log.Lines();
  ASSERT_EQ(lines.size(), 2u);
  for (const auto& line : lines) {
    EXPECT_NE(line.find("files"), std::string::npos);
    EXPECT_NE(line.find("controlled snapshot failure"), std::string::npos);
  }
}

// Purpose: The first change is measured against the baseline the caller
// took before acknowledging the start, so a change made between that
// snapshot and the first poll is reported, even after failed polls.
TEST_F(PollingObserversTest, ReportsTheFirstChangeAgainstTheBaseline) {
  ControlledSource source;
  source.Set({{"a", 1}, {"b", 1}});
  source.SetReadOutcome({.failing = true});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();
  EXPECT_TRUE(events_.empty());

  source.SetReadOutcome({.failing = false});
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b"}));
}

// Purpose: A change undone before delivery is not reported.
TEST_F(PollingObserversTest, ReportsNothingForAChangeUndoneBeforeDelivery) {
  ControlledSource source;
  source.Set({{"a", 1}});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});

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
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  const std::string observer_id =
      observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});
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
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});
  ASSERT_TRUE(FillMessageQueue());

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));

  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b"}));
}

// Purpose: A change whose wake could not be queued is still delivered when
// every later poll fails, because a failed poll retries the pending wake.
TEST_F(PollingObserversTest, RetriesAWakeAfterFailedPolls) {
  ControlledSource source;
  source.Set({{"a", 1}});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});
  ASSERT_TRUE(FillMessageQueue());

  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.SetReadOutcome({.failing = true});
  ASSERT_TRUE(source.WaitForPollToFinish());
  // Empties the queue; no wake is in it, because every post was refused.
  PumpQueuedMessages();

  ASSERT_TRUE(PumpUntil([&]() { return !events_.empty(); }, kDeliveryTimeout));
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(IdsOf(events_[0].event), (std::vector<std::string>{"b"}));
}

// Purpose: Stopping does not wait out a long polling interval.
TEST_F(PollingObserversTest, StopsPromptlyDuringALongPollingInterval) {
  ControlledSource source;
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  const std::string observer_id = observers.Start(SpecFor(source, kLongPoll), {});

  EXPECT_LT(TimeOf([&]() { observers.Stop(observer_id); }), kPromptTeardown);
}

// Purpose: Teardown tells a snapshot in progress to stop.
TEST_F(PollingObserversTest, TeardownStopsASnapshotInProgress) {
  std::atomic<bool> scanning{false};
  std::atomic<bool> saw_stop{false};
  DetachedThreadsDrain drain;
  auto observers = std::make_unique<PollingObservers>(RecordingEffects());
  observers->Start({.domain = "files",
                    .polling_interval = kFastPoll,
                    .take_snapshot =
                        [&scanning, &saw_stop](const StopRequested& stop_requested) {
                          scanning.store(true);
                          // Stands in for a scan that runs until it is told to stop.
                          const auto give_up = std::chrono::steady_clock::now() + 10s;
                          while (!stop_requested() &&
                                 std::chrono::steady_clock::now() < give_up) {
                            std::this_thread::sleep_for(1ms);
                          }
                          saw_stop.store(stop_requested());
                          return Snapshot{};
                        }},
                   {});
  ASSERT_TRUE(WaitWithoutPumping([&]() { return scanning.load(); },
                                 kDeliveryTimeout));

  observers.reset();

  EXPECT_TRUE(WaitWithoutPumping([&]() { return saw_stop.load(); }, kDeliveryTimeout));
}

// Purpose: Teardown returns at once even while a snapshot ignores the stop
// request, as filesystem or store I/O can, and that snapshot's result is
// never delivered.
TEST_F(PollingObserversTest, TeardownDoesNotWaitForASnapshotThatIgnoresStop) {
  std::atomic<bool> scanning{false};
  std::atomic<bool> released{false};
  DetachedThreadsDrain drain;
  auto observers = std::make_unique<PollingObservers>(RecordingEffects());
  observers->Start({.domain = "files",
                    .polling_interval = kFastPoll,
                    .take_snapshot =
                        [&scanning, &released](const StopRequested&) {
                          scanning.store(true);
                          // Stands in for blocking I/O that cannot be cancelled.
                          while (!released.load()) {
                            std::this_thread::sleep_for(1ms);
                          }
                          return Snapshot{{"b", 1}};
                        }},
                   {});
  ASSERT_TRUE(WaitWithoutPumping([&]() { return scanning.load(); },
                                 kDeliveryTimeout));

  EXPECT_LT(TimeOf([&]() { observers.reset(); }), kPromptTeardown);

  released.store(true);
  ASSERT_TRUE(WaitWithoutPumping([]() { return RunningDetachedThreads() == 0; },
                                 kDeliveryTimeout));
  PumpQueuedMessages();
  EXPECT_TRUE(events_.empty());
}

// Purpose: Only one event per observer waits for Dart at a time. Changes
// found meanwhile fold into one pending change, sent once Dart answers.
TEST_F(PollingObserversTest, WaitsForDartsReplyAndThenSendsOneFoldedEvent) {
  hold_replies_ = true;
  ControlledSource source;
  source.Set({{"a", 1}});
  DetachedThreadsDrain drain;
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll), {{"a", 1}});
  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(PumpUntil([&]() { return events_.size() == 1; }, kDeliveryTimeout));

  source.Set({{"a", 1}, {"b", 1}, {"c", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  source.Set({{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();
  ASSERT_EQ(events_.size(), 1u);

  ASSERT_EQ(held_replies_.size(), 1u);
  held_replies_[0]();
  ASSERT_TRUE(PumpUntil([&]() { return events_.size() == 2; }, kDeliveryTimeout));
  EXPECT_EQ(IdsOf(events_[1].event), (std::vector<std::string>{"c", "d"}));
  ASSERT_TRUE(source.WaitForPollToFinish());
  PumpQueuedMessages();
  EXPECT_EQ(events_.size(), 2u);
}

// Purpose: Teardown with a change queued for delivery sends nothing later.
TEST_F(PollingObserversTest, TeardownDropsAQueuedChange) {
  ControlledSource source;
  source.Set({{"a", 1}});
  DetachedThreadsDrain drain;
  auto observers = std::make_unique<PollingObservers>(RecordingEffects());
  observers->Start(SpecFor(source, kFastPoll), {{"a", 1}});
  source.Set({{"a", 1}, {"b", 1}});
  ASSERT_TRUE(source.WaitForPollToFinish());

  observers.reset();
  PumpQueuedMessages();

  EXPECT_TRUE(events_.empty());
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
