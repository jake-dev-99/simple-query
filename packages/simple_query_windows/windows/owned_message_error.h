#ifndef SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_
#define SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_

#include <string>
#include <utility>

namespace simple_query_windows {

// Purpose: Raise a standard exception whose message the exception itself
// owns. Flutter's Windows build sets _HAS_EXCEPTIONS=0, and in that mode
// MSVC's standard exceptions keep only a pointer to the text they are built
// from. A message built at run time would then point at freed memory once
// unwinding destroys the string it came from.
// @param Base is the standard exception the error is caught as, such as
// std::runtime_error.
template <typename Base>
class OwnedMessageError : public Base {
 public:
  // Purpose: Build the error.
  // @param message says what failed.
  // @returns Nothing.
  // @throws std::bad_alloc when memory runs out.
  explicit OwnedMessageError(std::string message)
      // The base gets a literal because what() below never reads its copy.
      : Base(""), message_(std::move(message)) {}

  // Purpose: Give the message the error was built with.
  // @returns The message, valid for the error's whole lifetime.
  // @throws Nothing.
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_
