#ifndef SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_

#include <flutter/encodable_value.h>

#include <filesystem>
#include <functional>
#include <string>

#include "native_query.g.h"

namespace simple_query_windows {

// Purpose: Name one files or media mutation.
struct FileMutation {
  // The mutation request: type, values, filters and platformData.
  const flutter::EncodableMap& request;
  // "files" or "media".
  const std::string& domain;
  // Finds the directory a filtered delete or update searches; it throws
  // std::system_error when the directory cannot be read.
  const std::function<std::filesystem::path()>& resolve_root;
};

// Purpose: Insert, update or delete files or media. A filtered delete or
// update targets the rows a query with the same filters returns: their path
// for files, and their uriOrPath for media.
// @param mutation names the request, its domain and its root.
// @returns The affected count, plus insertedId for an insert;
// invalid-query for a malformed request; unavailable when a filesystem step
// fails, naming the step and the path.
// @throws Nothing; failures are returned as errors.
ErrorOr<flutter::EncodableMap> MutateFiles(const FileMutation& mutation);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_
