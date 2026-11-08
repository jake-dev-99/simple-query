#ifndef SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_

#include <flutter/encodable_value.h>

#include <filesystem>
#include <string>
#include <vector>

#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Select what ListRecords walks and when it gives up.
struct ListRecordsOptions {
  std::filesystem::path root;
  // Lists only image, video and audio files, as media rows.
  bool media_only = false;
  // Ends the walk early when it returns true. Empty means never.
  StopRequested stop_requested;
};

// Purpose: Lowercase the ASCII letters of a string.
// @param value is the string to lowercase.
// @returns The string with A to Z replaced by a to z.
// @throws Nothing.
std::string Lower(std::string value);

// Purpose: Guess a file's MIME type from its extension.
// @param path is the file's path.
// @returns The MIME type, or application/octet-stream when unknown.
// @throws Nothing.
std::string MimeFromPath(const std::filesystem::path& path);

// Purpose: List the files under a root directory as query rows.
// @param options selects the root, the row shape and an early stop.
// @returns One row per entry found, or the rows found before a stop. An entry
// whose details cannot be read, such as one removed during the walk, is left
// out.
// @throws std::system_error when an entry's path cannot be converted to the
// system code page, which happens for some non-ASCII names.
std::vector<flutter::EncodableMap> ListRecords(const ListRecordsOptions& options);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_
