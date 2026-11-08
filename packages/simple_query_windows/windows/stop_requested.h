#ifndef SIMPLE_QUERY_WINDOWS_STOP_REQUESTED_H_
#define SIMPLE_QUERY_WINDOWS_STOP_REQUESTED_H_

#include <functional>

namespace simple_query_windows {

// Returns true once the work that received it should end early, such as an
// observer snapshot whose observer is stopping. An empty function means the
// work never stops early.
using StopRequested = std::function<bool()>;

// Purpose: Report whether work given a stop check should end now.
// @param stop_requested is the work's stop check; empty means never.
// @returns true once the work should end.
// @throws Nothing beyond what stop_requested throws.
inline bool IsStopRequested(const StopRequested& stop_requested) {
  return stop_requested && stop_requested();
}

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_STOP_REQUESTED_H_
