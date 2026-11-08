#ifndef SIMPLE_QUERY_WINDOWS_DEFERRED_HOST_CALL_H_
#define SIMPLE_QUERY_WINDOWS_DEFERRED_HOST_CALL_H_

#include <exception>
#include <functional>
#include <utility>

#include "background_calls.h"
#include "native_query.g.h"
#include "owned_message_error.h"
#include "structured_log.h"

namespace simple_query_windows {

// Purpose: Own one host call split across worker and platform threads.
// @param work reads retained inputs on the worker; standard exceptions become unavailable.
// @param finish runs on the platform thread with the work's value.
// @param result sends exactly one reply on the platform thread, unless torn down.
// @returns Nothing; this is a data type.
// @throws Nothing; callbacks document their own contracts.
template <typename Value, typename Reply>
struct BackgroundCall {
  std::function<Value(const StopRequested&)> work;
  std::function<ErrorOr<Reply>(Value)> finish;
  std::function<void(ErrorOr<Reply>)> result;
};

// Purpose: Catch deferred native failures before they can escape a worker thread.
// @param call owns its inputs and the platform-thread reply callback.
// @returns Work producing a success or unavailable reply closure for platform dispatch.
// @throws std::bad_alloc if allocating the work closure itself fails.
template <typename Value, typename Reply>
BackgroundJob MakeBackgroundJob(BackgroundCall<Value, Reply> call) {
  return [work = std::move(call.work), finish = std::move(call.finish),
          result = std::move(call.result)](const StopRequested& stop_requested)
             -> std::function<void()> {
    try {
      Value value = work(stop_requested);
      return [finish, result, value = std::move(value)]() mutable {
        result(finish(std::move(value)));
      };
    } catch (const std::exception& error) {
      const std::string message = ExceptionText(error);
      WriteLog({LogLevel::kError, "host_work_failed", {{"error", message}}});
      const FlutterError failure("unavailable", "simple_query: windows host work failed - " + message);
      return [result, failure]() { result(failure); };
    }
  };
}

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_DEFERRED_HOST_CALL_H_
