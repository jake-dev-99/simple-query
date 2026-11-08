#include "background_calls.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "detached_threads.h"
#include "platform_thread_dispatcher.h"
#include "structured_log.h"

namespace simple_query_windows {

// Purpose: Hold the queues the background worker and the platform thread
// share. The worker holds it by shared ownership, so it outlives the owner.
struct BackgroundCallsState {
  // Guards jobs, completions and unfinished, and orders a stop with the
  // worker's wait.
  std::mutex mutex;
  // Wakes the worker for a new job or a stop.
  std::condition_variable work_signal;
  std::deque<BackgroundJob> jobs;
  // Finishing steps waiting for the platform thread.
  std::deque<std::function<void()>> completions;
  // Closed by the owner, after which every wake request fails.
  std::shared_ptr<PlatformThreadDispatcher> dispatcher;
  // Set once by the owner's destructor. Read without the mutex by the stop
  // checks jobs poll.
  std::atomic<bool> stopping{false};
  // Calls queued and not yet finished on the platform thread.
  size_t unfinished = 0;
  size_t capacity = 0;
};

namespace {

// How long the worker waits before asking again for a platform-thread wake
// the message queue refused.
constexpr std::chrono::milliseconds kWakeRetryInterval{100};

// Purpose: Take the next job, waiting for one or for a stop. While a wake is
// owed the wait is bounded, so the worker can retry the wake.
// @param state is the shared queue state.
// @param wake_owed is true when a finishing step still needs a wake.
// @returns The next job, or nothing when stopping or when the bounded wait
// ended with no job.
// @throws Nothing.
std::optional<BackgroundJob> NextJob(BackgroundCallsState& state, bool wake_owed) {
  std::unique_lock<std::mutex> lock(state.mutex);
  const auto has_work = [&state]() { return state.stopping.load() || !state.jobs.empty(); };
  if (wake_owed) {
    state.work_signal.wait_for(lock, kWakeRetryInterval, has_work);
  } else {
    state.work_signal.wait(lock, has_work);
  }
  if (state.stopping.load() || state.jobs.empty()) {
    return std::nullopt;
  }
  BackgroundJob job = std::move(state.jobs.front());
  state.jobs.pop_front();
  return job;
}

// Purpose: Queue a finished job's platform-thread step.
// @param state is the shared queue state.
// @param completion is the step to run on the platform thread.
// @returns false when the owner is gone, so the step was dropped.
// @throws Nothing.
bool PushCompletion(BackgroundCallsState& state, std::function<void()> completion) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.stopping.load()) {
    return false;
  }
  state.completions.push_back(std::move(completion));
  return true;
}

// Purpose: Ask the platform thread to run queued steps, logging a failure
// once per run of failures.
// @param state is the shared queue state.
// @param wake_failing records whether the last request failed and was logged.
// @returns true when a wake is queued.
// @throws Nothing.
bool RequestWake(BackgroundCallsState& state, bool* wake_failing) {
  if (state.dispatcher->RequestWake()) {
    *wake_failing = false;
    return true;
  }
  // Read before logging, which can change the thread's last error.
  const DWORD error = GetLastError();
  if (!*wake_failing && !state.stopping.load()) {
    *wake_failing = true;
    WriteLog({LogLevel::kError,
              "background_wake_failed",
              {{"windows_error", std::to_string(error)},
               {"retry", "every " + std::to_string(kWakeRetryInterval.count()) + " ms"},
               {"next_log", "after a wake is queued"}}});
  }
  return false;
}

// Purpose: Run jobs in order until the owner is destroyed.
// @param state is the shared queue state, kept alive by this worker.
// @returns Nothing.
// @throws Nothing; jobs must not throw.
void RunWorker(const std::shared_ptr<BackgroundCallsState>& state) {
  const StopRequested stop_requested = [state]() { return state->stopping.load(); };
  bool wake_owed = false;
  bool wake_failing = false;
  while (!state->stopping.load()) {
    std::optional<BackgroundJob> job = NextJob(*state, wake_owed);
    if (job.has_value()) {
      if (!PushCompletion(*state, (*job)(stop_requested))) {
        return;
      }
      wake_owed = true;
    }
    if (wake_owed) {
      wake_owed = !RequestWake(*state, &wake_failing);
    }
  }
}

}  // namespace

// Purpose: Start the worker and the platform-thread wake.
// @param capacity is the most calls that may be unfinished at once.
// @returns Nothing; a failed setup makes every Enqueue return kNotReady.
// @throws Nothing; a failed setup is logged.
BackgroundCalls::BackgroundCalls(size_t capacity)
    : state_(std::make_shared<BackgroundCallsState>()) {
  state_->capacity = capacity;
  state_->dispatcher =
      std::make_shared<PlatformThreadDispatcher>([this]() { RunCompletions(); });
  if (!state_->dispatcher->IsReady()) {
    WriteLog({LogLevel::kError,
              "background_calls_unavailable",
              {{"reason", "the platform-thread message window could not be created"}}});
    return;
  }
  try {
    StartDetachedThread([state = state_]() { RunWorker(state); });
  } catch (const std::system_error& error) {
    // Without a worker no call can run, so the wake is closed and every
    // Enqueue reports kNotReady.
    state_->dispatcher->Close();
    WriteLog({LogLevel::kError,
              "background_calls_unavailable",
              {{"reason", "the worker thread could not start"}, {"error", error.what()}}});
  }
}

// Purpose: Stop the worker without waiting for it, dropping every call not
// yet finished.
// @returns Nothing.
// @throws Nothing.
BackgroundCalls::~BackgroundCalls() {
  std::deque<BackgroundJob> dropped_jobs;
  std::deque<std::function<void()>> dropped_completions;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping.store(true);
    dropped_jobs.swap(state_->jobs);
    dropped_completions.swap(state_->completions);
  }
  state_->work_signal.notify_all();
  // Closed here, on the platform thread, so the worker's later wake requests
  // fail instead of reaching a step that uses this destroyed object.
  state_->dispatcher->Close();
}

// Purpose: Queue one call for the worker.
// @param job is the call's background step.
// @returns kQueued when it will run; kFull or kNotReady when it was not
// queued, and job is dropped without running.
// @throws Nothing.
EnqueueResult BackgroundCalls::Enqueue(BackgroundJob job) {
  if (!state_->dispatcher->IsReady()) {
    return EnqueueResult::kNotReady;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->unfinished >= state_->capacity) {
      return EnqueueResult::kFull;
    }
    ++state_->unfinished;
    state_->jobs.push_back(std::move(job));
  }
  state_->work_signal.notify_one();
  return EnqueueResult::kQueued;
}

// Purpose: Run the finishing steps the worker produced. Runs on the platform
// thread.
// @returns Nothing.
// @throws Nothing beyond what a finishing step throws.
void BackgroundCalls::RunCompletions() {
  std::deque<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ready.swap(state_->completions);
    state_->unfinished -= ready.size();
  }
  // Run outside the lock: a step may queue another call.
  for (auto& completion : ready) {
    completion();
  }
}

}  // namespace simple_query_windows
