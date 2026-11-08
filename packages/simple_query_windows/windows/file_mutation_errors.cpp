#include "file_mutation_errors.h"

#include <variant>

#include "request_values.h"
#include "rows_result.h"

namespace simple_query_windows {
namespace file_mutation_detail {

using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Describe a failed filesystem step as an unavailable error.
// @param failure names the step, the path and the system error.
// @returns The error.
// @throws std::system_error when the path is not valid Unicode.
FlutterError FilesystemError(const FilesystemFailure& failure) {
  return FlutterError("unavailable", DescribeFilesystemFailure(failure));
}

// Purpose: Read the confirmed lower bound already attached to a failure.
// @param error is a mutation failure.
// @returns The confirmed count, or zero when no count has been attached.
// @throws Nothing.
int64_t ConfirmedEffects(const FlutterError& error) {
  if (const auto* details = std::get_if<EncodableMap>(&error.details())) {
    if (const auto* count = FindValue(*details, "affectedCount")) {
      if (const auto* value = std::get_if<int64_t>(count)) {
        return *value;
      }
    }
  }
  return 0;
}

// Purpose: Attach counts to an error, preserving any effects already reported.
// @param failure names the error and additional confirmed effects.
// @returns The error with details; the failing OS step may also have effects.
// @throws Nothing.
FlutterError WithAffected(const MutationFailure& failure) {
  EncodableMap details;
  const int64_t affected = failure.affected + ConfirmedEffects(failure.error);
  if (const auto* previous = std::get_if<EncodableMap>(&failure.error.details())) {
    details = *previous;
  }
  details[EncodableValue("affectedCount")] = EncodableValue(affected);
  details[EncodableValue("partialEffectsPossible")] = EncodableValue(true);
  return FlutterError(failure.error.code(), failure.error.message(), EncodableValue(details));
}

// Purpose: Report a stopped mutation and the effects completed before stopping.
// @param affected is the confirmed count before the stop.
// @returns An unavailable error with explicit stopped text and effect details.
// @throws Nothing.
FlutterError StoppedMutation(int64_t affected) {
  return WithAffected({FlutterError("unavailable", kReadStoppedError), affected});
}

// Purpose: Turn a requested stop into a failure with already confirmed effects.
// @param stop_requested reports cancellation; empty means never.
// @param affected is the confirmed effect count before stopping.
// @returns A stopped error, or no failure while the mutation remains active.
// @throws std::exception from the stop callback or error allocation.
std::optional<FlutterError> CheckMutationStop(const StopRequested& stop_requested,
                                             int64_t affected) {
  if (IsStopRequested(stop_requested)) return StoppedMutation(affected);
  return std::nullopt;
}

}  // namespace file_mutation_detail
}  // namespace simple_query_windows
