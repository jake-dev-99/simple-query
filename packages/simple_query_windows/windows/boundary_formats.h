#ifndef SIMPLE_QUERY_WINDOWS_BOUNDARY_FORMATS_H_
#define SIMPLE_QUERY_WINDOWS_BOUNDARY_FORMATS_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace simple_query_windows {

// Dart sends and expects UTF-8 text, while Windows paths are UTF-16 and the
// narrow std::filesystem conversions use the system code page. Every path
// that crosses the Flutter boundary goes through these two functions, so a
// name outside the code page reaches the right file and arrives intact.

// Purpose: Give a Windows path as the UTF-8 text Dart reads.
// @param path is the native path.
// @returns The path as UTF-8.
// @throws std::system_error when the path holds UTF-16 that is not valid
// Unicode, such as an unpaired surrogate in a file name.
std::string PathToUtf8(const std::filesystem::path& path);

// Purpose: Give the Windows path that UTF-8 text from Dart names.
// @param text is the path as UTF-8.
// @returns The native path.
// @throws std::system_error when the text is not valid UTF-8.
std::filesystem::path PathFromUtf8(const std::string& text);

// Purpose: Convert a Windows time, counted in 100-nanosecond ticks since
// 1601-01-01 UTC, to the system clock. FILETIME and Windows Runtime
// DateTime both count this way.
// @param ticks is the count of 100-nanosecond ticks since 1601-01-01 UTC.
// @returns The same instant on the system clock, to the millisecond.
// @throws Nothing.
std::chrono::system_clock::time_point TimeFromWindowsTicks(int64_t ticks);

// Purpose: Format an instant as the ISO 8601 UTC text Dart's DateTime.parse
// reads, such as 2026-11-08T04:21:42.123Z.
// @param instant is the instant to format.
// @returns The instant in UTC with millisecond precision and a Z suffix.
// @throws Nothing.
std::string FormatIsoUtc(std::chrono::system_clock::time_point instant);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_BOUNDARY_FORMATS_H_
