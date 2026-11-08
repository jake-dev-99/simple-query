#ifndef SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_

#include <flutter/encodable_value.h>

#include <filesystem>
#include <string>

#include "rows_result.h"
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
// @throws std::system_error when the extension is not valid Unicode.
std::string MimeFromPath(const std::filesystem::path& path);

// Purpose: List the files under a root directory as query rows. Paths are
// UTF-8 and times are ISO 8601 UTC, as Dart reads them.
// @param options selects the root, the row shape and an early stop.
// @returns One row per entry, or an error when the walk could not finish:
// a directory or entry that cannot be read, a name that is not valid
// Unicode, or a stop. An entry removed during the walk is left out, and a
// root that does not exist has no rows.
// @throws Nothing; failures are returned in error.
RowsResult ListRecords(const ListRecordsOptions& options);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_RECORDS_H_
