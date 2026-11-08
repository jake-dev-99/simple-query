#ifndef SIMPLE_QUERY_WINDOWS_FILE_MUTATION_ERRORS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_MUTATION_ERRORS_H_

#include <cstdint>
#include <optional>

#include "boundary_formats.h"
#include "native_query.g.h"
#include "stop_requested.h"

namespace simple_query_windows {
namespace file_mutation_detail {

// Purpose: Add confirmed effects to a failure without claiming rollback.
// @param error is the returned error.
// @param affected is the number of completed targets.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct MutationFailure {
  const FlutterError& error;
  int64_t affected;
};

// Purpose: Describe a failed filesystem step as an unavailable error.
// @param failure names the step, the path and the system error.
// @returns The error.
// @throws std::system_error when the path is not valid Unicode.
FlutterError FilesystemError(const FilesystemFailure& failure);

// Purpose: Read the confirmed lower bound already attached to a failure.
// @param error is a mutation failure.
// @returns The confirmed count, or zero when no count has been attached.
// @throws Nothing.
int64_t ConfirmedEffects(const FlutterError& error);

// Purpose: Attach counts to an error, preserving any effects already reported.
// @param failure names the error and additional confirmed effects.
// @returns The error with details; the failing OS step may also have effects.
// @throws Nothing.
FlutterError WithAffected(const MutationFailure& failure);

// Purpose: Report a stopped mutation and the effects completed before stopping.
// @param affected is the confirmed count before the stop.
// @returns An unavailable error with explicit stopped text and effect details.
// @throws Nothing.
FlutterError StoppedMutation(int64_t affected);

// Purpose: Turn a requested stop into a failure with already confirmed effects.
// @param stop_requested reports cancellation; empty means never.
// @param affected is the confirmed effect count before stopping.
// @returns A stopped error, or no failure while the mutation remains active.
// @throws std::exception from the stop callback or error allocation.
std::optional<FlutterError> CheckMutationStop(const StopRequested& stop_requested,
                                             int64_t affected = 0);

}  // namespace file_mutation_detail
}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_MUTATION_ERRORS_H_
