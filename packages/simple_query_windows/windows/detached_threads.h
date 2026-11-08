#ifndef SIMPLE_QUERY_WINDOWS_DETACHED_THREADS_H_
#define SIMPLE_QUERY_WINDOWS_DETACHED_THREADS_H_

#include <cstdint>
#include <functional>

namespace simple_query_windows {

// Purpose: Run work on a thread nobody joins, so the platform thread never
// waits for filesystem or store I/O that ignores a stop request.
//
// The work must hold, by shared ownership or by copy, everything it touches,
// because its starter may be gone before it finishes. The plugin DLL is a
// load-time dependency of the runner, so its code stays mapped until the
// process exits and a late-finishing thread never runs unloaded code.
// @param work is the thread's body. It must not throw; a throw ends the
// process.
// @returns Nothing.
// @throws std::system_error when the thread cannot be started; work then
// never runs.
void StartDetachedThread(std::function<void()> work);

// Purpose: Count the detached threads that have not finished, including
// destroying everything their work held.
// @returns The number of unfinished detached threads.
// @throws Nothing.
int64_t RunningDetachedThreads();

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_DETACHED_THREADS_H_
