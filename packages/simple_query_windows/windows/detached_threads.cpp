#include "detached_threads.h"

#include <atomic>
#include <system_error>
#include <thread>
#include <utility>

namespace simple_query_windows {

namespace {

// Purpose: Hold the process-wide count of unfinished detached threads.
// @returns The counter.
// @throws Nothing.
std::atomic<int64_t>& RunningCount() {
  static std::atomic<int64_t> count{0};
  return count;
}

}  // namespace

// Purpose: Run work on a thread nobody joins.
// @param work is the thread's body; it must not throw.
// @returns Nothing.
// @throws std::system_error when the thread cannot be started.
void StartDetachedThread(std::function<void()> work) {
  RunningCount().fetch_add(1);
  try {
    std::thread([body = std::move(work)]() mutable {
      {
        // Moved into a scope that ends before the count drops, so whatever
        // the work held is destroyed before a waiter sees the thread done.
        std::function<void()> owned = std::move(body);
        owned();
      }
      RunningCount().fetch_sub(1);
    }).detach();
  } catch (const std::system_error&) {
    // No thread started, so nothing will lower the count for it.
    RunningCount().fetch_sub(1);
    throw;
  }
}

// Purpose: Count the detached threads that have not finished.
// @returns The number of unfinished detached threads.
// @throws Nothing.
int64_t RunningDetachedThreads() { return RunningCount().load(); }

}  // namespace simple_query_windows
