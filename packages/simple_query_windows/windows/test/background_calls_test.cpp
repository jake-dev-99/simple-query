// Native tests for the background worker that runs slow host calls off the
// platform thread and finishes each one on it.

#include "background_calls.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using namespace std::chrono_literals;

constexpr size_t kCapacity = 4;

// Purpose: Build a job that records which thread ran each step.
// @param work_thread receives the thread the background step ran on.
// @param finish_thread receives the thread the finishing step ran on.
// @returns The job.
// @throws Nothing.
BackgroundJob RecordThreads(std::shared_ptr<std::thread::id> work_thread,
                            std::shared_ptr<std::thread::id> finish_thread) {
  return [work_thread, finish_thread](const StopRequested&) -> std::function<void()> {
    *work_thread = std::this_thread::get_id();
    return [finish_thread]() { *finish_thread = std::this_thread::get_id(); };
  };
}

// Purpose: Build a job that blocks the worker until released, ignoring the
// stop request, as blocking store or filesystem I/O can.
// @param started turns true once the job runs.
// @param released lets the job finish.
// @returns The job; its finishing step does nothing.
// @throws Nothing.
BackgroundJob BlockUntilReleased(std::shared_ptr<std::atomic<bool>> started,
                                 std::shared_ptr<std::atomic<bool>> released) {
  return [started, released](const StopRequested&) -> std::function<void()> {
    started->store(true);
    while (!released->load()) {
      std::this_thread::sleep_for(1ms);
    }
    return []() {};
  };
}

// Purpose: A job's background step runs off the platform thread and its
// finishing step runs on it.
TEST(BackgroundCallsTest, RunsWorkOffThePlatformThreadAndFinishesOnIt) {
  DetachedThreadsDrain drain;
  BackgroundCalls calls(kCapacity);
  auto work_thread = std::make_shared<std::thread::id>();
  auto finish_thread = std::make_shared<std::thread::id>();

  ASSERT_EQ(calls.Enqueue(RecordThreads(work_thread, finish_thread)),
            EnqueueResult::kQueued);
  ASSERT_TRUE(PumpUntil([&]() { return *finish_thread != std::thread::id(); },
                        kDeliveryTimeout));

  EXPECT_NE(*work_thread, std::this_thread::get_id());
  EXPECT_EQ(*finish_thread, std::this_thread::get_id());
}

// Purpose: Jobs run, and finish, in the order they were queued.
TEST(BackgroundCallsTest, KeepsTheOrderCallsWereMadeIn) {
  constexpr int kJobs = 3;
  auto worked = std::make_shared<std::vector<int>>();
  std::vector<int> finished;
  DetachedThreadsDrain drain;
  BackgroundCalls calls(kCapacity);

  for (int job = 0; job < kJobs; ++job) {
    ASSERT_EQ(calls.Enqueue([worked, job, &finished](const StopRequested&) {
      // Only the worker touches worked, one job at a time.
      worked->push_back(job);
      return std::function<void()>([job, &finished]() { finished.push_back(job); });
    }),
              EnqueueResult::kQueued);
  }
  ASSERT_TRUE(PumpUntil([&]() { return finished.size() == static_cast<size_t>(kJobs); }, kDeliveryTimeout));

  EXPECT_EQ(*worked, (std::vector<int>{0, 1, 2}));
  EXPECT_EQ(finished, (std::vector<int>{0, 1, 2}));
}

// Purpose: Calls beyond the cap are refused until earlier ones finish, so a
// stalled worker cannot grow memory without limit.
TEST(BackgroundCallsTest, RefusesCallsBeyondTheCapUntilOneFinishes) {
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto released = std::make_shared<std::atomic<bool>>(false);
  int finished = 0;
  DetachedThreadsDrain drain;
  BackgroundCalls calls(kCapacity);
  ASSERT_EQ(calls.Enqueue(BlockUntilReleased(started, released)), EnqueueResult::kQueued);
  for (size_t queued = 1; queued < kCapacity; ++queued) {
    ASSERT_EQ(calls.Enqueue([&finished](const StopRequested&) {
      return std::function<void()>([&finished]() { ++finished; });
    }),
              EnqueueResult::kQueued);
  }

  EXPECT_EQ(calls.Enqueue([](const StopRequested&) { return std::function<void()>([]() {}); }),
            EnqueueResult::kFull);

  released->store(true);
  ASSERT_TRUE(PumpUntil([&]() { return finished == static_cast<int>(kCapacity) - 1; },
                        kDeliveryTimeout));
  EXPECT_EQ(calls.Enqueue([](const StopRequested&) { return std::function<void()>([]() {}); }),
            EnqueueResult::kQueued);
}

// Purpose: Destroying the owner returns at once while a job blocks, and the
// job's finishing step never runs.
TEST(BackgroundCallsTest, TearsDownWithoutWaitingAndDropsTheFinishingStep) {
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto released = std::make_shared<std::atomic<bool>>(false);
  bool finished = false;
  DetachedThreadsDrain drain;
  auto calls = std::make_unique<BackgroundCalls>(kCapacity);
  ASSERT_EQ(calls->Enqueue([started, released, &finished](const StopRequested&) {
    started->store(true);
    while (!released->load()) {
      std::this_thread::sleep_for(1ms);
    }
    return std::function<void()>([&finished]() { finished = true; });
  }),
            EnqueueResult::kQueued);
  ASSERT_TRUE(WaitWithoutPumping([&]() { return started->load(); }, kDeliveryTimeout));

  EXPECT_LT(TimeOf([&]() { calls.reset(); }), kPromptTeardown);

  released->store(true);
  ASSERT_TRUE(WaitWithoutPumping([]() { return RunningDetachedThreads() == 0; },
                                 kDeliveryTimeout));
  PumpQueuedMessages();
  EXPECT_FALSE(finished);
}

// Purpose: A job's stop check turns true once the owner is destroyed, so
// long work can end early.
TEST(BackgroundCallsTest, TellsARunningJobToStopOnTeardown) {
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto saw_stop = std::make_shared<std::atomic<bool>>(false);
  DetachedThreadsDrain drain;
  auto calls = std::make_unique<BackgroundCalls>(kCapacity);
  ASSERT_EQ(calls->Enqueue([started, saw_stop](const StopRequested& stop_requested) {
    started->store(true);
    const auto give_up = std::chrono::steady_clock::now() + 10s;
    while (!stop_requested() && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(1ms);
    }
    saw_stop->store(stop_requested());
    return std::function<void()>([]() {});
  }),
            EnqueueResult::kQueued);
  ASSERT_TRUE(WaitWithoutPumping([&]() { return started->load(); }, kDeliveryTimeout));

  calls.reset();

  EXPECT_TRUE(WaitWithoutPumping([&]() { return saw_stop->load(); }, kDeliveryTimeout));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
