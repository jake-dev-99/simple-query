#ifndef SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_
#define SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Hold the queues the background worker and the platform thread
// share. Defined in background_calls.cpp.
struct BackgroundCallsState;

// Every call includes its queue wait in this deadline.
inline constexpr std::chrono::milliseconds kHostCallTimeout{30000};

// Runs on the background worker and returns the step that finishes the call
// on the platform thread, such as sending its reply. It must not throw, and
// it must hold by copy or shared ownership everything it touches, because
// the owner can be destroyed while it runs. Its stop check turns true once
// the owner is destroyed, so long work can end early.
using BackgroundJob = std::function<std::function<void()>(const StopRequested&)>;

// Purpose: Name the monotonic clock used by deadlines.
// @param none.
// @returns A monotonic time point; providers must be thread-safe and not throw.
// @throws Nothing.
using MonotonicNow = std::function<std::chrono::steady_clock::time_point()>;

class PlatformThreadDispatcher;

// Purpose: Supply deadline policy and the worker's external dependencies.
// @param timeout includes time waiting in the queue and must be positive.
// @param now is the thread-safe monotonic clock provider.
// @param make_dispatcher creates a platform-thread dispatcher for the supplied
// callback; empty selects the Windows window and deadline timer. It must not throw.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct BackgroundCallOptions {
  std::chrono::milliseconds timeout{kHostCallTimeout};
  MonotonicNow now = []() { return std::chrono::steady_clock::now(); };
  std::function<std::shared_ptr<PlatformThreadDispatcher>(std::function<void()>)> make_dispatcher;
};

// Purpose: Say whether a call was accepted for the background worker.
enum class EnqueueResult {
  // The job will run, and its finishing step will run on the platform thread
  // unless the owner is destroyed first.
  kQueued,
  // Too many calls are waiting or awaiting their finishing step.
  kFull,
  // The worker or its platform-thread wake could not be set up.
  kNotReady,
};

// Purpose: Run slow host calls on one background worker and finish each on
// the platform thread that created this object.
//
// One worker runs every call in the order it was queued, so Dart sees the
// effects of its calls in the order it made them. A slow call therefore
// delays the calls queued behind it; the order is worth that wait. The
// number of calls not yet finished is capped, so a stalled store or
// platform thread cannot grow memory without limit. Every call has a
// deadline from enqueue. Expiry stops the worker and fails all unfinished
// calls; new work is refused until the owner is recreated. An uninterruptible
// filesystem operation may still finish, so a timeout does not undo its effects.
//
// The worker is detached. Destroying this object never waits for it: a call
// still running finishes on its own and its finishing step is dropped.
//
// Create, call and destroy this object on the platform thread.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class BackgroundCalls {
 public:
  // Purpose: Start the worker and the platform-thread wake.
  // @param capacity is the most calls that may be unfinished at once.
  // @param options supplies the deadline policy, clock and dispatcher factory.
  // @returns Nothing; a failed setup makes every Enqueue return kNotReady.
  // @throws Nothing; a failed setup is logged.
  explicit BackgroundCalls(size_t capacity,
                           BackgroundCallOptions options = {});

  // Purpose: Stop the worker without waiting for it, dropping every call
  // not yet finished.
  // @returns Nothing.
  // @throws Nothing.
  ~BackgroundCalls();

  BackgroundCalls(const BackgroundCalls&) = delete;
  BackgroundCalls& operator=(const BackgroundCalls&) = delete;

  // Purpose: Queue one call for the worker.
  // @param job is the call's background step.
  // @param on_timeout runs on the platform thread if any pending call expires.
  // Empty is permitted for work with no reply; late completions are dropped.
  // @returns kQueued when it will run; kFull or kNotReady when it was not
  // queued, and job is dropped without running.
  // @throws Nothing.
  EnqueueResult Enqueue(BackgroundJob job, std::function<void()> on_timeout = {});

 private:
  // Purpose: Check deadlines, then run finishing steps on the platform thread.
  // A timeout settles every unfinished call and stops further work.
  // @returns Nothing.
  // @throws Nothing beyond what a finishing step throws.
  void RunCompletions();

  std::shared_ptr<BackgroundCallsState> state_;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_
