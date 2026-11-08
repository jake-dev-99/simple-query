#ifndef SIMPLE_QUERY_WINDOWS_ROWS_RESULT_H_
#define SIMPLE_QUERY_WINDOWS_ROWS_RESULT_H_

#include <flutter/encodable_value.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "owned_message_error.h"

namespace simple_query_windows {

// The error a read reports when it stops before it finishes, so its partial
// rows are never taken for the whole store.
inline constexpr char kReadStoppedError[] =
    "simple_query: the read stopped before it finished";

// Purpose: Hold the rows a read returned, or why it could not finish.
// @param rows are complete only when error is empty.
// @param error is set when the read failed or stopped early; rows is then
// incomplete and must not be used.
// @param denied_capability is set with error when the user has not granted
// access: the Windows capability that would grant it, such as "contacts".
// @returns Nothing; this is a data type.
// @throws Nothing.
struct RowsResult {
  std::vector<flutter::EncodableMap> rows;
  std::optional<std::string> error;
  std::optional<std::string> denied_capability;
};

// Purpose: Report a read the user has not granted access to, naming the
// Windows capability that would grant it.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class ReadDeniedError : public OwnedMessageError<std::runtime_error> {
 public:
  // Purpose: Build the error.
  // @param message says what was denied and how to allow it.
  // @param capability is the Windows capability, such as "contacts".
  // @returns Nothing.
  // @throws std::bad_alloc when memory runs out.
  ReadDeniedError(std::string message, std::string capability)
      : OwnedMessageError<std::runtime_error>(std::move(message)),
        capability_(std::move(capability)) {}

  // Purpose: Name the capability that would grant the read.
  // @returns The Windows capability, such as "contacts".
  // @throws Nothing.
  const std::string& capability() const noexcept { return capability_; }

 private:
  std::string capability_;
};

// Purpose: Give a read's rows, or fail when the read could not finish, so a
// failed read is never mistaken for an empty one.
// @param result is the read's outcome.
// @returns The rows read.
// @throws ReadDeniedError when the user has not granted access, and
// std::runtime_error carrying the read's error when it failed otherwise.
std::vector<flutter::EncodableMap> RowsOrThrow(RowsResult result);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_ROWS_RESULT_H_
