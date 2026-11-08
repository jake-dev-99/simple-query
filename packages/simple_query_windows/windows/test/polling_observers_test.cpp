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

// Purpose: A failed poll reports nothing, so a passing read error never
// reports records as removed and added again, and later changes still arrive.
TEST_F(PollingObserversTest, SkipsAFailedPollAndKeepsReporting) {
  ControlledSource source;
  source.Set({{"a", 1}, {"b", 1}});
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(WaitForBaseline(source));

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
    PollingObservers observers(RecordingEffects());
    observers.Start(SpecFor(source, kFastPoll));
    ASSERT_TRUE(WaitForBaseline(source));

    source.SetReadOutcome({.failing = true});
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetReadOutcome({.failing = false});
    ASSERT_TRUE(source.WaitForPollToFinish());
    source.SetReadOutcome({.failing = true});
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
  source.SetReadOutcome({.failing = true});
  PollingObservers observers(RecordingEffects());
  observers.Start(SpecFor(source, kFastPoll));
  ASSERT_TRUE(source.WaitForPollToFinish());

  source.SetReadOutcome({.failing = false});
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

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
