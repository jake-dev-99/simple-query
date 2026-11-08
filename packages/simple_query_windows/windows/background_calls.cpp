#include "background_calls.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
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

// Purpose: Keep a call alive while its worker can outlive its timeout reply.
// @param id identifies this call in the unfinished map.
// @param deadline includes time waiting in the queue.
// @param work runs only on the worker and must not throw.
// @param on_timeout is read only by the platform thread.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PendingCall {
  uint64_t id;
  std::chrono::steady_clock::time_point deadline;
  BackgroundJob work;
  std::function<void()> on_timeout;
};

// Purpose: Carry a completed call back to the platform thread.
// @param id identifies the unfinished call this settles.
// @param finish sends the reply only if the call has not timed out.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct CallCompletion {
  uint64_t id;
  std::function<void()> finish;
};

// Purpose: Hold bounded queues and every call still awaiting its reply.
// @param jobs wait in arrival order, preserving mutation ordering.
// @param completions hold finished work until the platform thread takes it.
// @param pending also counts work awaiting a reply, bounding all held state.
// @param capacity is the maximum unfinished count.
// @param next_id increases on each accepted call, ordering deadlines too.
// @param timeout is fixed before the worker starts.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct CallQueues {
  std::deque<std::shared_ptr<PendingCall>> jobs;
  std::deque<CallCompletion> completions;
  std::map<uint64_t, std::shared_ptr<PendingCall>> pending;
  size_t capacity = 0;
  uint64_t next_id = 0;
  std::chrono::milliseconds timeout{0};
};

// Purpose: Keep worker state valid after owner destruction or a timed-out I/O.
// @param mutex guards queues and orders stopping with the worker wait.
// @param work_signal wakes the worker for work, a timeout or teardown.
// @param queues contains all unfinished work, under mutex.
// @param dispatcher posts completions and checks deadlines on its timer.
// @param stopping cancels cooperative operations and prevents later work.
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

constexpr std::chrono::milliseconds kWakeRetryInterval{100};
constexpr std::chrono::milliseconds kDeadlineCheckInterval{10};

// Purpose: Track a refused platform-thread wake until a later post succeeds.
// @param owed limits the worker's next wait so it retries a failed post.
// @param failing suppresses repeated logs until a post succeeds.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct WakeState {
  bool owed = false;
  bool failing = false;
};

// Purpose: Check the oldest deadline while the state mutex is held.
// @param queues holds calls in enqueue order, which is also deadline order.
// @returns true once an unfinished call reaches its deadline.
// @throws Nothing.
bool DeadlineReached(const CallQueues& queues) {
  return !queues.pending.empty() &&
         std::chrono::steady_clock::now() >= queues.pending.begin()->second->deadline;
}

// Purpose: Take the next job, waiting for work or a stop, with wake retries.
// @param state is the shared state; its mutex is taken here.
// @param wake bounds the wait while a platform-thread post is owed.
// @returns The next call, or null when stopped or no call is ready.
// @throws Nothing.
std::shared_ptr<PendingCall> NextJob(BackgroundCallsState& state, const WakeState& wake) {
  std::unique_lock<std::mutex> lock(state.mutex);
  const auto has_work = [&state]() {
    return state.stopping.load() || !state.queues.jobs.empty();
  };
  if (wake.owed) {
    state.work_signal.wait_for(lock, kWakeRetryInterval, has_work);
  } else {
    state.work_signal.wait(lock, has_work);
  }
  if (DeadlineReached(state.queues)) {
    // The platform timer will settle these calls. Do not start another
    // mutation while an earlier call already has an unknown outcome.
    state.stopping.store(true);
  }
  if (state.stopping.load() || state.queues.jobs.empty()) {
    return nullptr;
  }
  auto job = std::move(state.queues.jobs.front());
  state.queues.jobs.pop_front();
  return job;
}

// Purpose: Queue a reply only while its call is still within its deadline.
// @param state is the shared state; its mutex is taken here.
// @param call identifies the call and its deadline.
// @param completion is the platform-thread reply step.
// @returns false when stopped or expired, dropping any late result.
// @throws Nothing.
bool PushCompletion(BackgroundCallsState& state, const PendingCall& call,
                    std::function<void()> completion) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (DeadlineReached(state.queues)) {
    state.stopping.store(true);
  }
  if (state.stopping.load()) {
    return false;
  }
  state.queues.completions.push_back({call.id, std::move(completion)});
  return true;
}

// Purpose: Retry a completion wake, logging one failure per streak.
// @param state is the shared state; only its dispatcher and stop flag are read.
// @param wake records whether a post is owed and its failure already logged.
// @returns Nothing.
// @throws Nothing.
void RequestWake(BackgroundCallsState& state, WakeState& wake) {
  if (state.dispatcher->RequestWake()) {
    wake = WakeState{};
    return;
  }
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

// Purpose: Run queued work in order until stopped, including on a deadline.
// @param state is kept alive by this detached worker.
// @returns Nothing.
// @throws Nothing; jobs must not throw.
void RunWorker(const std::shared_ptr<BackgroundCallsState>& state) {
  WakeState wake;
  while (!state->stopping.load()) {
    auto call = NextJob(*state, wake);
    if (call) {
      const StopRequested stop_requested = [state, deadline = call->deadline]() {
        return state->stopping.load() || std::chrono::steady_clock::now() >= deadline;
      };
      if (!PushCompletion(*state, *call, call->work(stop_requested))) {
        return;
      }
      wake.owed = true;
    }
    if (wake.owed) {
      RequestWake(*state, wake);
    }
  }
}

// Purpose: Record an unavailable worker or deadline mechanism.
// @param reason identifies the setup failure.
// @returns Nothing.
// @throws Nothing.
void LogUnavailable(const std::string& reason) {
  WriteLog({LogLevel::kError, "background_calls_unavailable", {{"reason", reason}}});
}

}  // namespace

// Purpose: Start one worker and a platform-thread deadline timer.
// @param capacity bounds unfinished calls, including replies awaiting dispatch.
// @param timeout bounds each call from enqueue and must be positive.
// @returns Nothing; failed setup leaves Enqueue returning kNotReady.
// @throws Nothing; setup failures are logged.
BackgroundCalls::BackgroundCalls(size_t capacity, std::chrono::milliseconds timeout)
    : state_(std::make_shared<BackgroundCallsState>()) {
  state_->queues.capacity = capacity;
  state_->queues.timeout = timeout;
  state_->dispatcher = std::make_shared<PlatformThreadDispatcher>(
      [this]() { RunCompletions(); }, kDeadlineCheckInterval);
  if (!state_->dispatcher->IsReady()) {
    LogUnavailable("the platform-thread message window or deadline timer could not start");
    return;
  }
  if (timeout.count() <= 0) {
    state_->dispatcher->Close();
    LogUnavailable("the host-call deadline must be positive");
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
    state_->dispatcher->Close();
    LogUnavailable(*failure);
  }
}

// Purpose: Stop without waiting for I/O and drop every remaining reply.
// @param none.
// @returns Nothing.
// @throws Nothing.
BackgroundCalls::~BackgroundCalls() {
  CallQueues dropped;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping.store(true);
    std::swap(dropped, state_->queues);
  }
  state_->work_signal.notify_all();
  state_->dispatcher->Close();
}

// Purpose: Accept one bounded call with its deadline reply.
// @param job is the worker step, holding its dependencies by shared ownership.
// @param on_timeout runs on the platform thread on expiry; empty means no reply.
// @returns kQueued, kFull, or kNotReady after setup failure or timeout.
// @throws Nothing.
EnqueueResult BackgroundCalls::Enqueue(BackgroundJob job, std::function<void()> on_timeout) {
  if (!state_->dispatcher->IsReady()) {
    return EnqueueResult::kNotReady;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping.load()) {
      return EnqueueResult::kNotReady;
    }
    CallQueues& queues = state_->queues;
    if (queues.pending.size() >= queues.capacity) {
      return EnqueueResult::kFull;
    }
    const uint64_t id = ++queues.next_id;
    auto call = std::make_shared<PendingCall>(PendingCall{
        id, std::chrono::steady_clock::now() + queues.timeout,
        std::move(job), std::move(on_timeout)});
    queues.pending.emplace(id, call);
    queues.jobs.push_back(std::move(call));
  }
  state_->work_signal.notify_one();
  return EnqueueResult::kQueued;
}

// Purpose: Settle expired calls, or deliver completed calls before their deadline.
// @param none.
// @returns Nothing; timeout disables this owner until recreated.
// @throws Nothing beyond what a reply callback throws.
void BackgroundCalls::RunCompletions() {
  std::deque<CallCompletion> ready;
  CallQueues expired;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (DeadlineReached(state_->queues)) {
      state_->stopping.store(true);
      std::swap(expired, state_->queues);
    } else {
      ready.swap(state_->queues.completions);
      for (const auto& completion : ready) {
        state_->queues.pending.erase(completion.id);
      }
    }
  }
  if (!expired.pending.empty()) {
    // Timer and replies run on the platform thread. Neither needs the
    // blocked worker; its retained state keeps an eventual return safe.
    state_->work_signal.notify_all();
    state_->dispatcher->Close();
    WriteLog({LogLevel::kError,
              "background_calls_timed_out",
              {{"timeout_ms", std::to_string(expired.timeout.count())},
               {"unfinished_calls", std::to_string(expired.pending.size())},
               {"outcome", "unfinished operations may still finish"}}});
    for (const auto& entry : expired.pending) {
      if (entry.second->on_timeout) {
        entry.second->on_timeout();
      }
    }
    return;
  }
  for (auto& completion : ready) {
    completion.finish();
  }
}

}  // namespace simple_query_windows
