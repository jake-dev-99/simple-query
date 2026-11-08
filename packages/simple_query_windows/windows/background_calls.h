#ifndef SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_
#define SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_

#include <cstddef>
#include <functional>
#include <memory>

#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Hold the queues the background worker and the platform thread
// share. Defined in background_calls.cpp.
struct BackgroundCallsState;

// Runs on the background worker and returns the step that finishes the call
// on the platform thread, such as sending its reply. It must not throw, and
// it must hold by copy or shared ownership everything it touches, because
// the owner can be destroyed while it runs. Its stop check turns true once
// the owner is destroyed, so long work can end early.
using BackgroundJob = std::function<std::function<void()>(const StopRequested&)>;

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
// platform thread cannot grow memory without limit.
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
  // @returns Nothing; a failed setup makes every Enqueue return kNotReady.
  // @throws Nothing; a failed setup is logged.
  explicit BackgroundCalls(size_t capacity);

  // Purpose: Stop the worker without waiting for it, dropping every call
  // not yet finished.
  // @returns Nothing.
  // @throws Nothing.
  ~BackgroundCalls();

  BackgroundCalls(const BackgroundCalls&) = delete;
  BackgroundCalls& operator=(const BackgroundCalls&) = delete;

  // Purpose: Queue one call for the worker.
  // @param job is the call's background step.
  // @returns kQueued when it will run; kFull or kNotReady when it was not
  // queued, and job is dropped without running.
  // @throws Nothing.
  EnqueueResult Enqueue(BackgroundJob job);

 private:
  // Purpose: Run the finishing steps the worker produced. Runs on the
  // platform thread.
  // @returns Nothing.
  // @throws Nothing beyond what a finishing step throws.
  void RunCompletions();

  std::shared_ptr<BackgroundCallsState> state_;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_BACKGROUND_CALLS_H_
