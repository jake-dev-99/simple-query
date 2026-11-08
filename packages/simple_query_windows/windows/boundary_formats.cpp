#include "boundary_formats.h"

#include <cstdio>

#include "windows_text.h"

namespace simple_query_windows {

namespace {

// Ticks between 1601-01-01 and 1970-01-01, the two clocks' epochs.
constexpr int64_t kWindowsToUnixEpochTicks = 116444736000000000LL;
// One Windows tick is 100 nanoseconds.
constexpr int64_t kTicksPerMillisecond = 10000;
// Holds "YYYY-MM-DDTHH:MM:SS.mmmZ" and its terminator, with room for a
// five-digit year.
constexpr size_t kIsoBufferSize = 32;

}  // namespace

// Purpose: Give a Windows path as the UTF-8 text Dart reads.
// @param path is the native path.
// @returns The path as UTF-8.
// @throws std::system_error when the path holds UTF-16 that is not valid
// Unicode, such as an unpaired surrogate in a file name.
std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// Purpose: Give the Windows path that UTF-8 text from Dart names.
// @param text is the path as UTF-8.
// @returns The native path.
// @throws std::system_error when the text is not valid UTF-8.
std::filesystem::path PathFromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// Purpose: Convert a Windows time, counted in 100-nanosecond ticks since
// 1601-01-01 UTC, to the system clock.
// @param ticks is the count of 100-nanosecond ticks since 1601-01-01 UTC.
// @returns The same instant on the system clock, to the millisecond.
// @throws Nothing.
std::chrono::system_clock::time_point TimeFromWindowsTicks(int64_t ticks) {
  const std::chrono::milliseconds since_unix_epoch(
      (ticks - kWindowsToUnixEpochTicks) / kTicksPerMillisecond);
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(since_unix_epoch));
}

// Purpose: Format an instant as the ISO 8601 UTC text Dart's DateTime.parse
// reads, such as 2026-11-08T04:21:42.123Z.
// @param instant is the instant to format.
// @returns The instant in UTC with millisecond precision and a Z suffix.
// @throws Nothing.
std::string FormatIsoUtc(std::chrono::system_clock::time_point instant) {
  using namespace std::chrono;
  const auto milliseconds_point = floor<milliseconds>(instant);
  const auto day = floor<days>(milliseconds_point);
  const year_month_day date{day};
  const hh_mm_ss<milliseconds> time{milliseconds_point - day};
  char buffer[kIsoBufferSize];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return buffer;
}

// Purpose: Describe a failed filesystem step in the text Dart receives.
// @param failure names the step, the path and the system error.
// @returns The error text.
// @throws std::system_error when the path holds UTF-16 that is not valid
// Unicode or Windows cannot convert the error diagnostic.
std::string DescribeFilesystemFailure(const FilesystemFailure& failure) {
  return "simple_query: windows filesystem " + std::string(failure.operation) +
         " failed for " + PathToUtf8(failure.path) + " - " + ErrorCodeTextToUtf8(failure.error);
}

}  // namespace simple_query_windows
