#include "file_mutations.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "boundary_formats.h"
#include "file_records.h"
#include "query_shaping.h"
#include "request_values.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
namespace fs = std::filesystem;

// Purpose: Describe a failed filesystem step as an unavailable error.
// @param operation names the step, such as "delete".
// @param path is the path the step acted on.
// @param error is the system error.
// @returns The error.
// @throws std::system_error when the path is not valid Unicode.
FlutterError FilesystemError(const char* operation, const fs::path& path,
                             const std::error_code& error) {
  return FlutterError("unavailable", "simple_query: windows filesystem " +
                                         std::string(operation) + " failed for " +
                                         PathToUtf8(path) + " - " + error.message());
}

// Purpose: Build the result of a mutation that changed some records.
// @param affected is how many records changed.
// @returns The result map Dart reads.
// @throws Nothing.
EncodableMap AffectedResult(int64_t affected) {
  EncodableMap result;
  result[EncodableValue("affectedCount")] = EncodableValue(affected);
  return result;
}

// Purpose: Create a file's parent directory when it is missing.
// @param path is the file's path.
// @returns Nothing on success, or the error.
// @throws std::system_error when the path is not valid Unicode.
std::optional<FlutterError> CreateParent(const fs::path& path) {
  const fs::path parent = path.parent_path();
  if (parent.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  fs::create_directories(parent, error);
  if (error) {
    return FilesystemError("create directory", parent, error);
  }
  return std::nullopt;
}

// Purpose: Replace a file's contents.
// @param path is the file's path.
// @param bytes are the new contents.
// @returns Nothing on success, or the error.
// @throws std::system_error when the path is not valid Unicode.
std::optional<FlutterError> WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (stream.is_open()) {
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
  }
  if (!stream.is_open() || !stream) {
    return FlutterError("unavailable",
                        "simple_query: windows filesystem write failed for " + PathToUtf8(path) +
                            ". Check that the file is not open elsewhere and the folder "
                            "is writable.");
  }
  return std::nullopt;
}

// Purpose: Read the new contents an update or insert carries.
// @param values are the mutation values: bytes as a Uint8List, or content
// as text.
// @returns The contents, nothing when the values carry none, or an
// invalid-query error for bytes that are not a Uint8List.
// @throws Nothing.
std::variant<std::optional<std::string>, FlutterError> NewContents(const EncodableMap& values) {
  if (const auto* bytes_value = FindValue(values, "bytes")) {
    const auto* bytes = std::get_if<std::vector<uint8_t>>(bytes_value);
    if (bytes == nullptr) {
      return FlutterError("invalid-query",
                          "simple_query: update expects values.bytes as Uint8List when provided");
    }
    return std::optional<std::string>(std::string(bytes->begin(), bytes->end()));
  }
  return StringAt(values, "content");
}

// Purpose: Create one file or directory.
// @param request is the insert request; values.path names the target.
// @returns The affected count and insertedId, or an error.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<EncodableMap> Insert(const EncodableMap& request) {
  const EncodableMap* values = MapAt(request, "values");
  if (values == nullptr) {
    return FlutterError("invalid-query", "simple_query: insert requires values");
  }
  const auto path_text = StringAt(*values, "path");
  if (!path_text.has_value() || path_text->empty()) {
    return FlutterError("invalid-query", "simple_query: insert requires values.path");
  }
  const fs::path path = PathFromUtf8(*path_text);
  if (BoolAt(*values, "isDirectory") == true) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) {
      return FilesystemError("create directory", path, error);
    }
  } else {
    auto contents = NewContents(*values);
    if (auto* invalid = std::get_if<FlutterError>(&contents)) {
      return *invalid;
    }
    if (auto failed = CreateParent(path)) {
      return *failed;
    }
    // An insert without bytes or content creates an empty file.
    const auto& text = std::get<std::optional<std::string>>(contents);
    if (auto failed = WriteBytes(path, text.value_or(std::string()))) {
      return *failed;
    }
  }
  EncodableMap result = AffectedResult(1);
  result[EncodableValue("insertedId")] = EncodableValue(*path_text);
  return result;
}

// Purpose: Find the paths a filtered delete or update targets.
// @param mutation names the request, its domain and its root.
// @returns The paths of the rows a query with the same filters returns, or
// an error when the filters are invalid or the folder cannot be read.
// @throws std::system_error when the root cannot be read or a path is not
// valid Unicode.
ErrorOr<std::vector<fs::path>> FilteredTargets(const FileMutation& mutation) {
  auto filters = ParseFilters(mutation.request);
  if (auto* invalid = std::get_if<std::string>(&filters)) {
    return FlutterError("invalid-query", *invalid);
  }
  const bool media = mutation.domain == "media";
  RowsResult listed = ListRecords({.root = mutation.resolve_root(), .media_only = media});
  if (listed.error.has_value()) {
    return FlutterError("unavailable", *listed.error);
  }
  const auto rows = ApplyFilters(std::move(listed.rows),
                                 std::get<std::vector<QueryFilter>>(filters));
  // Media rows carry their path as uriOrPath; file rows as path.
  const std::string path_key = media ? "uriOrPath" : "path";
  std::vector<fs::path> targets;
  for (const auto& row : rows) {
    const auto path = StringAt(row, path_key);
    if (path.has_value() && !path->empty()) {
      targets.push_back(PathFromUtf8(*path));
    }
  }
  return targets;
}

// Purpose: Delete the targets of a filtered delete.
// @param mutation names the request, its domain and its root.
// @returns How many targets were removed, or an error.
// @throws std::system_error when the root cannot be read or a path is not
// valid Unicode.
ErrorOr<EncodableMap> Delete(const FileMutation& mutation) {
  auto targets = FilteredTargets(mutation);
  if (targets.has_error()) {
    return targets.error();
  }
  int64_t deleted = 0;
  for (const auto& target : targets.value()) {
    std::error_code error;
    // A target inside a folder deleted earlier is already gone, and
    // removes nothing.
    const auto removed = fs::remove_all(target, error);
    if (error) {
      return FilesystemError("delete", target, error);
    }
    if (removed > 0) {
      ++deleted;
    }
  }
  return AffectedResult(deleted);
}

// Purpose: Move one update target to values.newPath, when it names another
// path.
// @param target is the target's path; it becomes the new path on success.
// @param values are the update values.
// @returns Whether the target moved, or an error.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<bool> MoveTarget(fs::path* target, const EncodableMap& values) {
  const auto new_path_text = StringAt(values, "newPath");
  if (!new_path_text.has_value() || new_path_text->empty()) {
    return false;
  }
  const fs::path new_path = PathFromUtf8(*new_path_text);
  if (new_path == *target) {
    return false;
  }
  if (auto failed = CreateParent(new_path)) {
    return *failed;
  }
  std::error_code error;
  fs::rename(*target, new_path, error);
  if (error) {
    return FilesystemError("rename", *target, error);
  }
  *target = new_path;
  return true;
}

// Purpose: Apply an update to one target: move it, then replace a file's
// contents.
// @param original is the target's path.
// @param values are the update values.
// @returns Whether the target changed, false when it no longer exists, or
// an error.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<bool> UpdateOne(const fs::path& original, const EncodableMap& values) {
  std::error_code error;
  const bool exists = fs::exists(original, error);
  if (error) {
    return FilesystemError("metadata read", original, error);
  }
  if (!exists) {
    return false;
  }
  fs::path target = original;
  auto moved = MoveTarget(&target, values);
  if (moved.has_error()) {
    return moved.error();
  }
  const bool is_directory = fs::is_directory(target, error);
  if (error) {
    return FilesystemError("metadata read", target, error);
  }
  if (is_directory) {
    return moved.value();
  }
  auto contents = NewContents(values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) {
    return *invalid;
  }
  const auto& text = std::get<std::optional<std::string>>(contents);
  if (!text.has_value()) {
    return moved.value();
  }
  if (auto failed = WriteBytes(target, *text)) {
    return *failed;
  }
  return true;
}

// Purpose: Update the targets an update names: values.path, values.id, or
// else the rows its filters match.
// @param mutation names the request, its domain and its root.
// @returns How many targets changed, or an error.
// @throws std::system_error when the root cannot be read or a path is not
// valid Unicode.
ErrorOr<EncodableMap> Update(const FileMutation& mutation) {
  const EncodableMap* values = MapAt(mutation.request, "values");
  if (values == nullptr) {
    return FlutterError("invalid-query", "simple_query: update requires values");
  }
  std::vector<fs::path> targets;
  const auto explicit_path = StringAt(*values, "path");
  const auto explicit_id = StringAt(*values, "id");
  if (explicit_path.has_value() && !explicit_path->empty()) {
    targets.push_back(PathFromUtf8(*explicit_path));
  } else if (explicit_id.has_value() && !explicit_id->empty()) {
    // A file or media row's id is its path.
    targets.push_back(PathFromUtf8(*explicit_id));
  } else {
    auto filtered = FilteredTargets(mutation);
    if (filtered.has_error()) {
      return filtered.error();
    }
    targets = filtered.value();
  }

  int64_t updated = 0;
  for (const auto& target : targets) {
    auto changed = UpdateOne(target, *values);
    if (changed.has_error()) {
      return changed.error();
    }
    if (changed.value()) {
      ++updated;
    }
  }
  return AffectedResult(updated);
}

}  // namespace

// Purpose: Insert, update or delete files or media. A filtered delete or
// update targets the rows a query with the same filters returns: their path
// for files, and their uriOrPath for media.
// @param mutation names the request, its domain and its root.
// @returns The affected count, plus insertedId for an insert;
// invalid-query for a malformed request; unavailable when a filesystem step
// fails, naming the step and the path.
// @throws Nothing; failures are returned as errors.
ErrorOr<EncodableMap> MutateFiles(const FileMutation& mutation) {
  const auto type = StringAt(mutation.request, "type");
  try {
    if (type == "insert") {
      return Insert(mutation.request);
    }
    if (type == "delete") {
      return Delete(mutation);
    }
    if (type == "update") {
      return Update(mutation);
    }
  } catch (const std::system_error& error) {
    // Raised by a path that is not valid Unicode or an unreadable working
    // directory.
    return FlutterError("unavailable",
                        std::string("simple_query: windows filesystem mutation failed - ") +
                            error.what());
  }
  return FlutterError("invalid-query",
                      "simple_query: unknown mutation type " + type.value_or("(none)"));
}

}  // namespace simple_query_windows
