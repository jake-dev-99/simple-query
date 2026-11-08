#include "rows_result.h"

#include <stdexcept>
#include <utility>

namespace simple_query_windows {

// Purpose: Give a read's rows, or fail when the read could not finish, so a
// failed read is never mistaken for an empty one.
// @param result is the read's outcome.
// @returns The rows read.
// @throws ReadDeniedError when the user has not granted access, and
// std::runtime_error carrying the read's error when it failed otherwise.
std::vector<flutter::EncodableMap> RowsOrThrow(RowsResult result) {
  if (result.error.has_value()) {
    if (result.denied_capability.has_value()) {
      throw ReadDeniedError(std::move(*result.error), std::move(*result.denied_capability));
    }
    throw OwnedMessageError<std::runtime_error>(std::move(*result.error));
  }
  return std::move(result.rows);
}

}  // namespace simple_query_windows
