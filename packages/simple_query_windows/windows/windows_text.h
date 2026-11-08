#ifndef SIMPLE_QUERY_WINDOWS_WINDOWS_TEXT_H_
#define SIMPLE_QUERY_WINDOWS_WINDOWS_TEXT_H_

#include <windows.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace simple_query_windows {

// Purpose: Convert narrow Windows diagnostics to the UTF-8 Flutter expects.
// Windows standard-library error messages use the active ANSI code page;
// an explicit code page also permits deterministic conversion tests.
// @param text holds bytes in code_page, without requiring a terminator.
// @param code_page is the source Windows code page; CP_ACP selects the
// machine's active ANSI code page.
// @returns The same diagnostic encoded as UTF-8.
// @throws std::system_error when Windows cannot decode or encode the text,
// std::length_error when it exceeds Windows' conversion length limit, and
// std::bad_alloc when allocation fails.
inline std::string WindowsNarrowTextToUtf8(std::string_view text,
                                          unsigned int code_page = CP_ACP) {
  if (text.empty()) return {};
  if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    throw std::length_error("Windows diagnostic exceeds the conversion length limit");
  }
  const int length = static_cast<int>(text.size());
  const DWORD flags = code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
  const int wide_size = MultiByteToWideChar(code_page, flags, text.data(), length,
                                           nullptr, 0);
  if (wide_size == 0) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
  }
  std::wstring wide(static_cast<size_t>(wide_size), L'\0');
  if (MultiByteToWideChar(code_page, flags, text.data(), length, wide.data(),
                          wide_size) == 0) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
  }
  const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                           wide_size, nullptr, 0, nullptr, nullptr);
  if (utf8_size == 0) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
  }
  std::string utf8(static_cast<size_t>(utf8_size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size,
                          utf8.data(), utf8_size, nullptr, nullptr) == 0) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
  }
  return utf8;
}

// Purpose: Give an error code's diagnostic as UTF-8 while retaining the
// original category and value that select the diagnostic.
// @param error is the original standard-library error code.
// @param code_page is its message's encoding; Windows' generic and system
// categories use the active ANSI code page by default.
// @returns The category's message encoded as UTF-8.
// @throws std::system_error for conversion failure, std::length_error for
// oversized text, and std::bad_alloc when allocation fails.
inline std::string ErrorCodeTextToUtf8(const std::error_code& error,
                                      unsigned int code_page = CP_ACP) {
  return WindowsNarrowTextToUtf8(error.message(), code_page);
}

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_WINDOWS_TEXT_H_
