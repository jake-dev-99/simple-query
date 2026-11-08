#include "file_records.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

#include "boundary_formats.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Hold the creation and last write times of one entry.
struct EntryTimes {
  std::chrono::system_clock::time_point created;
  std::chrono::system_clock::time_point modified;
};

// Purpose: Hold what a row reports about one entry.
struct EntryInfo {
  bool is_directory = false;
  // Set for regular files only.
  std::optional<int64_t> size;
  EntryTimes times;
  std::string mime;
};

// Purpose: Hold the outcome of reading one entry: a row, nothing for an
// entry to leave out, or why the walk cannot finish.
struct EntryRead {
  std::optional<EncodableMap> row;
  std::optional<std::string> error;
};

// Purpose: Name one filesystem step that failed, for the walk's error.
struct ScanFailureDetail {
  const char* operation;
  const std::filesystem::path& path;
  const std::error_code& error;
};

// Purpose: Report whether a MIME type is an image, video or audio type.
// @param mime is the MIME type.
// @returns true for image/, video/ and audio/ types.
// @throws Nothing.
bool IsMediaMime(const std::string& mime) {
  return mime.rfind("image/", 0) == 0 || mime.rfind("video/", 0) == 0 ||
         mime.rfind("audio/", 0) == 0;
}

// Purpose: Name the media kind of a MIME type.
// @param mime is the MIME type.
// @returns "image", "video", "audio" or "other".
// @throws Nothing.
std::string MediaType(const std::string& mime) {
  if (mime.rfind("image/", 0) == 0) return "image";
  if (mime.rfind("video/", 0) == 0) return "video";
  if (mime.rfind("audio/", 0) == 0) return "audio";
  return "other";
}

// Purpose: Describe a failed filesystem step as the walk's error.
// @param detail names the step, the path and the system error.
// @returns The error text.
// @throws std::system_error when the path is not valid Unicode.
std::string ScanFailure(const ScanFailureDetail& detail) {
  return "simple_query: windows filesystem " + std::string(detail.operation) +
         " failed for " + PathToUtf8(detail.path) + " - " + detail.error.message();
}

// Purpose: Turn an entry's failed read into its outcome. An entry removed
// during the walk is left out; any other failure ends the walk, so a partial
// listing is never reported as complete.
// @param detail names the step, the path and the system error.
// @returns An empty outcome for a removed entry, otherwise an error.
// @throws std::system_error when the path is not valid Unicode.
EntryRead FailedOrRemoved(const ScanFailureDetail& detail) {
  if (detail.error == std::errc::no_such_file_or_directory) {
    return {};
  }
  return {.row = std::nullopt, .error = ScanFailure(detail)};
}

// Purpose: Convert a FILETIME to the system clock.
// @param time is the FILETIME.
// @returns The same instant on the system clock.
// @throws Nothing.
std::chrono::system_clock::time_point TimeFromFileTime(const FILETIME& time) {
  const uint64_t ticks =
      (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  return TimeFromWindowsTicks(static_cast<int64_t>(ticks));
}

// Purpose: Read an entry's creation and last write times. The standard
// library offers no creation time, and its last write time counts from 1601
// on MSVC, so both come from the same Windows call.
// @param path is the entry's path.
// @param error receives the system error when the times cannot be read.
// @returns The times, or nothing when they cannot be read.
// @throws Nothing.
std::optional<EntryTimes> ReadEntryTimes(const std::filesystem::path& path,
                                         std::error_code& error) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) {
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return std::nullopt;
  }
  return EntryTimes{.created = TimeFromFileTime(data.ftCreationTime),
                    .modified = TimeFromFileTime(data.ftLastWriteTime)};
}

// Purpose: Build the media row for one entry.
// @param path is the entry's path.
// @param info holds what the row reports.
// @returns The row.
// @throws std::system_error when the path is not valid Unicode.
EncodableMap MediaRow(const std::filesystem::path& path, const EntryInfo& info) {
  const std::string text = PathToUtf8(path);
  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(text);
  row[EncodableValue("uriOrPath")] = EncodableValue(text);
  row[EncodableValue("mediaType")] = EncodableValue(MediaType(info.mime));
  row[EncodableValue("mimeType")] = EncodableValue(info.mime);
  if (info.size.has_value()) {
    row[EncodableValue("size")] = EncodableValue(*info.size);
  }
  row[EncodableValue("createdAt")] = EncodableValue(FormatIsoUtc(info.times.created));
  row[EncodableValue("modifiedAt")] = EncodableValue(FormatIsoUtc(info.times.modified));
  return row;
}

// Purpose: Build the file row for one entry.
// @param path is the entry's path.
// @param info holds what the row reports.
// @returns The row.
// @throws std::system_error when the path is not valid Unicode.
EncodableMap FileRow(const std::filesystem::path& path, const EntryInfo& info) {
  const std::string text = PathToUtf8(path);
  const int64_t modified_epoch_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          info.times.modified.time_since_epoch())
          .count();
  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(text);
  row[EncodableValue("path")] = EncodableValue(text);
  row[EncodableValue("name")] = EncodableValue(PathToUtf8(path.filename()));
  row[EncodableValue("isDirectory")] = EncodableValue(info.is_directory);
  if (info.size.has_value()) {
    row[EncodableValue("size")] = EncodableValue(*info.size);
  }
  row[EncodableValue("modifiedAt")] = EncodableValue(FormatIsoUtc(info.times.modified));
  row[EncodableValue("mimeType")] = EncodableValue(info.mime);
  row[EncodableValue("type")] = EncodableValue(info.is_directory ? "directory" : "file");
  row[EncodableValue("extension")] = EncodableValue(PathToUtf8(path.extension()));
  row[EncodableValue("modifiedEpochMs")] = EncodableValue(modified_epoch_ms);
  return row;
}

// Purpose: Read one entry of the walk.
// @param entry is the directory entry.
// @param media_only lists only media files, as media rows.
// @returns The entry's row, nothing for an entry to leave out, or an error.
// @throws std::system_error when the path is not valid Unicode.
EntryRead ReadEntry(const std::filesystem::directory_entry& entry, bool media_only) {
  std::error_code error;
  const std::filesystem::path& path = entry.path();
  EntryInfo info;
  info.is_directory = entry.is_directory(error);
  if (error) {
    return FailedOrRemoved({"metadata read", path, error});
  }
  info.mime = MimeFromPath(path);
  const bool media_file = !info.is_directory && IsMediaMime(info.mime);
  if (media_only && !media_file) {
    return {};
  }
  const bool is_regular = entry.is_regular_file(error);
  if (error) {
    return FailedOrRemoved({"metadata read", path, error});
  }
  if (is_regular) {
    const auto size = entry.file_size(error);
    if (error) {
      return FailedOrRemoved({"size read", path, error});
    }
    info.size = static_cast<int64_t>(size);
  }
  const auto times = ReadEntryTimes(path, error);
  if (!times.has_value()) {
    return FailedOrRemoved({"time read", path, error});
  }
  info.times = *times;
  return {.row = media_only ? MediaRow(path, info) : FileRow(path, info),
          .error = std::nullopt};
}

// Purpose: Walk the root and add each entry's row to result.
// @param options selects the root, the row shape and an early stop.
// @param result receives the rows, or the error that ended the walk.
// @returns Nothing.
// @throws std::system_error when a path is not valid Unicode.
void ScanInto(const ListRecordsOptions& options, RowsResult* result) {
  std::error_code error;
  const bool root_exists = std::filesystem::exists(options.root, error);
  if (error) {
    result->error = ScanFailure({"query", options.root, error});
    return;
  }
  if (!root_exists) {
    return;
  }
  // No directory is skipped, so an unreadable folder fails the walk instead
  // of quietly shrinking it.
  std::filesystem::recursive_directory_iterator it(
      options.root, std::filesystem::directory_options::none, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && it != end) {
    // A recursive walk of a large tree can outlast an observer's teardown.
    if (IsStopRequested(options.stop_requested)) {
      result->error = kReadStoppedError;
      return;
    }
    EntryRead read = ReadEntry(*it, options.media_only);
    if (read.error.has_value()) {
      result->error = std::move(read.error);
      return;
    }
    if (read.row.has_value()) {
      result->rows.push_back(std::move(*read.row));
    }
    it.increment(error);
  }
  if (error) {
    result->error = ScanFailure({"query", options.root, error});
  }
}

}  // namespace

// Purpose: Lowercase the ASCII letters of a string.
// @param value is the string to lowercase.
// @returns The string with A to Z replaced by a to z.
// @throws Nothing.
std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Purpose: Guess a file's MIME type from its extension.
// @param path is the file's path.
// @returns The MIME type, or application/octet-stream when unknown.
// @throws std::system_error when the extension is not valid Unicode.
std::string MimeFromPath(const std::filesystem::path& path) {
  const std::string ext = Lower(PathToUtf8(path.extension()));
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".heic") return "image/heic";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".aac") return "audio/aac";
  if (ext == ".flac") return "audio/flac";
  if (ext == ".txt") return "text/plain";
  if (ext == ".json") return "application/json";
  return "application/octet-stream";
}

// Purpose: List the files under a root directory as query rows. Paths are
// UTF-8 and times are ISO 8601 UTC, as Dart reads them.
// @param options selects the root, the row shape and an early stop.
// @returns One row per entry, or an error when the walk could not finish:
// a directory or entry that cannot be read, a name that is not valid
// Unicode, or a stop. An entry removed during the walk is left out, and a
// root that does not exist has no rows.
// @throws Nothing; failures are returned in error.
RowsResult ListRecords(const ListRecordsOptions& options) {
  RowsResult result;
  try {
    ScanInto(options, &result);
  } catch (const std::system_error& error) {
    // Dart reads UTF-8, so a name that is not valid Unicode cannot be sent.
    result.error = std::string("simple_query: windows filesystem query found a "
                               "name that is not valid Unicode - ") +
                   error.what();
  }
  return result;
}

}  // namespace simple_query_windows
