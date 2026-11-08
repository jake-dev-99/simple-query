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
#include "owned_message_error.h"

namespace simple_query_windows {

// Purpose: Hold the work the worker and the platform thread pass each
// other. BackgroundCallsState::mutex guards every field.
// @param jobs wait in arrival order; the worker takes the oldest first.
// @param completions run on the platform thread in the order their jobs
// finished.
// @param unfinished rises when a call is queued and falls only after its
// finishing step is taken, so it also counts calls awaiting their reply.
// @param capacity is fixed at construction. A call that would raise
// unfinished past it is refused, not queued.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct CallQueues {
  std::deque<BackgroundJob> jobs;
  std::deque<std::function<void()>> completions;
  size_t unfinished = 0;
  size_t capacity = 0;
};

// Purpose: Hold the state the background worker and the platform thread
// share. The worker holds it by shared ownership, so it outlives the owner.
// @param mutex guards queues, and orders a stop with the worker's wait.
// @param work_signal wakes the worker for a new job or a stop.
// @param queues is read and written only while mutex is held.
// @param dispatcher wakes the platform thread. The owner closes it, after
// which every wake request fails.
// @param stopping is set once by the owner's destructor. The stop checks
// jobs poll read it without the mutex.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct BackgroundCallsState {
  std::mutex mutex;
  std::condition_variable work_signal;
  CallQueues queues;
  std::shared_ptr<PlatformThreadDispatcher> dispatcher;
  std::atomic<bool> stopping{false};
};

namespace {

// How long the worker waits before asking again for a platform-thread wake
// the message queue refused.
constexpr std::chrono::milliseconds kWakeRetryInterval{100};

// Purpose: Track the worker's platform-thread wake. A refused wake is owed
// until the message queue takes one, and its failure is logged once per run.
// @param owed stays true from a refused wake until a wake is queued. While
// it is true the worker waits at most kWakeRetryInterval for a job.
// @param failing stops repeat logs: it is set when a refused wake is
// logged, and cleared when a wake is queued.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct WakeState {
  bool owed = false;
  bool failing = false;
};

// Purpose: Take the next job, waiting for one or for a stop. While a wake is
// owed the wait is bounded, so the worker can retry the wake.
// @param state is the shared state; this function locks its mutex.
// @param wake bounds the wait to kWakeRetryInterval while a wake is owed.
// @returns The next job, or nothing when stopping or when the bounded wait
// ended with no job.
// @throws Nothing.
std::optional<BackgroundJob> NextJob(BackgroundCallsState& state, const WakeState& wake) {
  std::unique_lock<std::mutex> lock(state.mutex);
  const auto has_work = [&state]() {
    return state.stopping.load() || !state.queues.jobs.empty();
  };
  if (wake.owed) {
    state.work_signal.wait_for(lock, kWakeRetryInterval, has_work);
  } else {
    state.work_signal.wait(lock, has_work);
  }
  if (state.stopping.load() || state.queues.jobs.empty()) {
    return std::nullopt;
  }
  BackgroundJob job = std::move(state.queues.jobs.front());
  state.queues.jobs.pop_front();
  return job;
}

// Purpose: Queue a finished job's platform-thread step.
// @param state is the shared state; this function locks its mutex.
// @param completion is the step to run on the platform thread.
// @returns false when the owner is gone, so the step was dropped.
// @throws Nothing.
bool PushCompletion(BackgroundCallsState& state, std::function<void()> completion) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.stopping.load()) {
    return false;
  }
  state.queues.completions.push_back(std::move(completion));
  return true;
}

// Purpose: Ask the platform thread to run queued steps, logging a failure
// once per run of failures.
// @param state is the shared state; only its dispatcher and stopping flag
// are read, so the mutex is not taken.
// @param wake is updated: owed clears once a wake is queued, and failing
// records whether this run of failures was logged.
// @returns Nothing.
// @throws Nothing.
void RequestWake(BackgroundCallsState& state, WakeState& wake) {
  if (state.dispatcher->RequestWake()) {
    wake = WakeState{};
    return;
  }
  // Read before logging, which can change the thread's last error.
  const DWORD error = GetLastError();
  if (!wake.failing && !state.stopping.load()) {
    wake.failing = true;
    WriteLog({LogLevel::kError,
              "background_wake_failed",
              {{"windows_error", std::to_string(error)},
               {"retry", "every " + std::to_string(kWakeRetryInterval.count()) + " ms"},
               {"next_log", "after a wake is queued"}}});
  }
}

// Purpose: Run jobs in order until the owner is destroyed.
// @param state is the shared queue state, kept alive by this worker.
// @returns Nothing.
// @throws Nothing; jobs must not throw.
void RunWorker(const std::shared_ptr<BackgroundCallsState>& state) {
  const StopRequested stop_requested = [state]() { return state->stopping.load(); };
  WakeState wake;
  while (!state->stopping.load()) {
    std::optional<BackgroundJob> job = NextJob(*state, wake);
    if (job.has_value()) {
      if (!PushCompletion(*state, (*job)(stop_requested))) {
        return;
      }
      wake.owed = true;
    }
    if (wake.owed) {
      RequestWake(*state, wake);
    }
  }
}

// Purpose: Log why the background worker is unavailable.
// @param reason says why the worker or its wake window could not start. It
// is written to the log as is.
// @returns Nothing.
// @throws Nothing.
void LogUnavailable(const std::string& reason) {
  WriteLog({LogLevel::kError, "background_calls_unavailable", {{"reason", reason}}});
}

}  // namespace

// Purpose: Start the worker and the platform-thread wake.
// @param capacity is fixed at construction. A call that would raise
// unfinished past it is refused, not queued.
// @returns Nothing; a failed setup makes every Enqueue return kNotReady.
// @throws Nothing; a failed setup is logged.
BackgroundCalls::BackgroundCalls(size_t capacity)
    : state_(std::make_shared<BackgroundCallsState>()) {
  state_->queues.capacity = capacity;
  state_->dispatcher =
      std::make_shared<PlatformThreadDispatcher>([this]() { RunCompletions(); });
  if (!state_->dispatcher->IsReady()) {
    LogUnavailable("the platform-thread message window could not be created");
    return;
  }
  std::optional<std::string> failure;
  try {
    if (StartDetachedThread([state = state_]() { RunWorker(state); }) ==
        DetachedStart::kAtLimit) {
      failure = "the process already runs " + std::to_string(kMaxDetachedThreads) +
                " worker threads";
    }
  } catch (const std::system_error& error) {
    failure = "the worker thread could not start: " + ExceptionText(error);
  }
  if (failure.has_value()) {
    // Without a worker no call can run, so the wake is closed and every
    // Enqueue reports kNotReady.
    state_->dispatcher->Close();
    LogUnavailable(*failure);
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
    dropped_jobs.swap(state_->queues.jobs);
    dropped_completions.swap(state_->queues.completions);
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
    CallQueues& queues = state_->queues;
    if (queues.unfinished >= queues.capacity) {
      return EnqueueResult::kFull;
    }
    ++queues.unfinished;
    queues.jobs.push_back(std::move(job));
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
    ready.swap(state_->queues.completions);
    state_->queues.unfinished -= ready.size();
  }
  // Run outside the lock: a step may queue another call.
  for (auto& completion : ready) {
    completion();
  }
}

}  // namespace simple_query_windows
