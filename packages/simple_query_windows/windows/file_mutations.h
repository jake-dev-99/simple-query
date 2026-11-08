#ifndef SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_

#include <flutter/encodable_value.h>

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

#include "native_query.g.h"
#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Name one staged replacement and its recovery location.
// @param original is the existing canonical file to replace.
// @param replacement is the completed staging file on the same volume.
// @param backup receives the original if replacement progresses past its rename.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileReplacement {
  const std::filesystem::path& original;
  const std::filesystem::path& replacement;
  const std::filesystem::path& backup;
};

// Purpose: Inject the narrow native write, flush and replacement boundaries in tests.
// @param write writes the complete bytes to the borrowed Windows HANDLE;
// empty uses WriteFile. It must return an error on any incomplete write.
// @param flush flushes that handle; empty uses FlushFileBuffers.
// @param replace publishes an update while preserving metadata; empty uses
// ReplaceFileW with merge errors enforced. Failure may leave original in backup.
// @returns Nothing; this is a data type.
// @throws Nothing; callbacks return failures as error codes.
struct FileWriteEffects {
  std::function<std::error_code(void*, const std::string&)> write;
  std::function<std::error_code(void*)> flush;
  std::function<std::error_code(const FileReplacement&)> replace;
};

// Purpose: Name one files or media mutation. The request, domain and root
// resolver are borrowed, so this must not outlive the call that builds it.
// @param request is Dart's decoded request. Its "type" picks insert, update
// or delete.
// @param domain picks the key a filtered step reads from each row:
// uriOrPath for "media", path for any other domain.
// @param resolve_root finds the folder a filtered delete or update searches.
// It throws std::system_error when the folder cannot be read.
// @param stop_requested ends scans and prevents subsequent mutation steps.
// @param write_effects supplies write, flush and replace boundaries; empty uses Windows.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileMutation {
  const flutter::EncodableMap& request;
  const std::string& domain;
  const std::function<std::filesystem::path()>& resolve_root;
  StopRequested stop_requested = {};
  FileWriteEffects write_effects = {};
};

// Purpose: Insert, update or delete files or media. A filtered delete or
// update targets the rows a query with the same filters returns: their path
// for files, and their uriOrPath for media.
// @param mutation names the request, its domain and its root.
// @returns The affected count, plus insertedId for an insert;
// invalid-query for a malformed request; unavailable when a filesystem step
// fails or the call stops. Unavailable error details carry affectedCount
// as a confirmed lower bound and partialEffectsPossible=true: earlier effects
// are not rolled back, and a failing filesystem step may have partial effects.
// A failed replacement/backup cleanup also reports recoveryPath; its directory
// is retained because it may hold the only original copy.
// @throws Nothing; failures are returned as errors.
ErrorOr<flutter::EncodableMap> MutateFiles(const FileMutation& mutation);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_
