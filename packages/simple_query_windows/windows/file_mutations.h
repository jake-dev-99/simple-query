#ifndef SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_
#define SIMPLE_QUERY_WINDOWS_FILE_MUTATIONS_H_

#include <flutter/encodable_value.h>

#include <filesystem>
#include <functional>
#include <string>

#include "native_query.g.h"

namespace simple_query_windows {

// Purpose: Name one files or media mutation. Every field is borrowed, so
// a mutation must not outlive the call that builds it.
// @param request is Dart's decoded request. Its "type" picks insert, update
// or delete.
// @param domain picks the key a filtered step reads from each row:
// uriOrPath for "media", path for any other domain.
// @param resolve_root finds the folder a filtered delete or update searches.
// It throws std::system_error when the folder cannot be read.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileMutation {
  const flutter::EncodableMap& request;
  const std::string& domain;
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
