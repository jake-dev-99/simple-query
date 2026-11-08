#ifndef SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_
#define SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_

#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace simple_query_windows {

// Purpose: Raise a standard exception whose message the exception itself
// owns. Flutter's Windows build sets _HAS_EXCEPTIONS=0, and in that mode
// MSVC's standard exceptions keep only a pointer to the text they are built
// from. A message built at run time would then point at freed memory once
// unwinding destroys the string it came from.
// @param Base is the standard exception the error is caught as, such as
// std::runtime_error.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
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

// Purpose: Give a caught exception's message as text that outlives it.
// With _HAS_EXCEPTIONS=0, std::system_error's what() points at a string
// destroyed while the error is built, so its code's message is used instead.
// @param error is the caught exception.
// @returns The error code's message for a std::system_error, otherwise
// what(), which this plugin's own errors and literal messages keep valid.
// @throws std::bad_alloc when memory runs out.
inline std::string ExceptionText(const std::exception& error) {
  if (const auto* system_error = dynamic_cast<const std::system_error*>(&error)) {
    return system_error->code().message();
  }
  return error.what();
}

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_OWNED_MESSAGE_ERROR_H_
