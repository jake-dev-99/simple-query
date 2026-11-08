#include "file_mutations.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
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
#include "owned_message_error.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
namespace fs = std::filesystem;

// Purpose: Describe a failed filesystem step as an unavailable error.
// @param failure names the step, the path and the system error.
// @returns The error.
// @throws std::system_error when the path is not valid Unicode.
FlutterError FilesystemError(const FilesystemFailure& failure) {
  return FlutterError("unavailable", DescribeFilesystemFailure(failure));
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
    return FilesystemError({"create directory", parent, error});
  }
  return std::nullopt;
}

// Purpose: Add confirmed effects to a failure without claiming rollback.
// @param error is the returned error.
// @param affected is the number of completed targets.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct MutationFailure {
  const FlutterError& error;
  int64_t affected;
};

// Purpose: Read the confirmed lower bound already attached to a failure.
// @param error is a mutation failure.
// @returns The confirmed count, or zero when no count has been attached.
// @throws Nothing.
int64_t ConfirmedEffects(const FlutterError& error) {
  if (const auto* details = std::get_if<EncodableMap>(&error.details())) {
    if (const auto* count = FindValue(*details, "affectedCount")) {
      if (const auto* value = std::get_if<int64_t>(count)) {
        return *value;
      }
    }
  }
  return 0;
}

// Purpose: Attach counts to an error, preserving any effects already reported.
// @param failure names the error and additional confirmed effects.
// @returns The error with details; the failing OS step may also have effects.
// @throws Nothing.
FlutterError WithAffected(const MutationFailure& failure) {
  EncodableMap details;
  const int64_t affected = failure.affected + ConfirmedEffects(failure.error);
  if (const auto* previous = std::get_if<EncodableMap>(&failure.error.details())) {
    details = *previous;
  }
  details[EncodableValue("affectedCount")] = EncodableValue(affected);
  details[EncodableValue("partialEffectsPossible")] = EncodableValue(true);
  return FlutterError(failure.error.code(), failure.error.message(), EncodableValue(details));
}

// Purpose: Ensure every unavailable outcome exposes confirmed partial effects.
// @param result is a completed mutation or a failure.
// @returns The result, attaching effect details to unavailable errors.
// @throws Nothing.
ErrorOr<EncodableMap> MutationOutcome(ErrorOr<EncodableMap> result) {
  if (result.has_error() && result.error().code() == "unavailable") {
    return WithAffected({result.error(), 0});
  }
  return result;
}

// Purpose: Report a stopped mutation and the effects completed before stopping.
// @param affected is the confirmed count before the stop.
// @returns An unavailable error with explicit stopped text and effect details.
// @throws Nothing.
FlutterError StoppedMutation(int64_t affected) {
  return WithAffected({FlutterError("unavailable", kReadStoppedError), affected});
}

// Purpose: Read the last Windows filesystem failure.
// @param none.
// @returns The captured system error.
// @throws Nothing.
std::error_code LastFileError() {
  return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

// Purpose: Describe one cancellable native write.
// @param handle is an open writable Windows HANDLE.
// @param bytes are the complete replacement contents.
// @param stop_requested is checked before every native write chunk.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct NativeWrite {
  void* handle;
  const std::string& bytes;
  const StopRequested& stop_requested;
};

// Purpose: Write every byte, checking cancellation before each Windows write chunk.
// @param write names the handle, contents and stop check.
// @returns No error on complete write, otherwise the write or cancellation error.
// @throws Nothing.
std::error_code WriteAll(const NativeWrite& write) {
  size_t offset = 0;
  while (offset < write.bytes.size()) {
    if (IsStopRequested(write.stop_requested)) {
      return std::make_error_code(std::errc::operation_canceled);
    }
    const auto chunk = static_cast<DWORD>(std::min<size_t>(
        write.bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(write.handle, write.bytes.data() + offset, chunk, &written, nullptr)) {
      return LastFileError();
    }
    if (written == 0) {
      return std::make_error_code(std::errc::io_error);
    }
    offset += written;
  }
  return {};
}

// Purpose: Name a complete staged write and its publication policy.
// @param path is the destination.
// @param bytes are its new contents.
// @param exclusive forbids replacing any existing destination for insert.
// @param mutation supplies stop checks and write/flush effects.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FileWrite {
  const fs::path& path;
  const std::string& bytes;
  bool exclusive;
  const FileMutation& mutation;
};

// Purpose: Stage a complete file beside its destination, then publish by rename.
// @param write names the destination, bytes and exclusive-create policy.
// @returns Nothing on success, or the write, flush, close, publish or stop error.
// Originals survive staging failures; temporary files are removed on failure.
// @throws std::system_error when the path is not valid Unicode.
std::optional<FlutterError> WriteBytes(const FileWrite& write) {
  if (IsStopRequested(write.mutation.stop_requested)) {
    return StoppedMutation(0);
  }
  fs::path destination = write.path;
  if (!write.exclusive) {
    std::error_code error;
    destination = fs::canonical(write.path, error);
    if (error) {
      return FilesystemError({"resolve update target", write.path, error});
    }
  }
  static std::atomic<uint64_t> next_temporary{0};
  fs::path temporary;
  HANDLE handle = INVALID_HANDLE_VALUE;
  // CREATE_NEW also handles stale or concurrently created staging names safely.
  while (handle == INVALID_HANDLE_VALUE) {
    if (IsStopRequested(write.mutation.stop_requested)) {
      return StoppedMutation(0);
    }
    temporary = destination;
    temporary += ".simple-query-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(next_temporary.fetch_add(1)) + ".tmp";
    handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const auto error = LastFileError();
      if (error.value() != static_cast<int>(ERROR_FILE_EXISTS)) {
        return FilesystemError({"create staging file", temporary, error});
      }
    }
  }
  std::optional<FlutterError> failure;
  if (IsStopRequested(write.mutation.stop_requested)) {
    failure = StoppedMutation(0);
  } else {
    const auto error = write.mutation.write_effects.write
                           ? write.mutation.write_effects.write(handle, write.bytes)
                           : WriteAll({.handle = handle, .bytes = write.bytes,
                                       .stop_requested = write.mutation.stop_requested});
    if (error == std::errc::operation_canceled) {
      failure = StoppedMutation(0);
    } else if (error) {
      failure = FilesystemError({"write staging file", temporary, error});
    }
  }
  if (!failure.has_value()) {
    if (IsStopRequested(write.mutation.stop_requested)) {
      failure = StoppedMutation(0);
    } else {
      const auto error = write.mutation.write_effects.flush
                             ? write.mutation.write_effects.flush(handle)
                             : (FlushFileBuffers(handle) ? std::error_code{} : LastFileError());
      if (error) {
        failure = FilesystemError({"flush staging file", temporary, error});
      }
    }
  }
  if (!CloseHandle(handle)) {
    failure = FilesystemError({"close staging file", temporary, LastFileError()});
  }
  if (!failure.has_value() && IsStopRequested(write.mutation.stop_requested)) {
    failure = StoppedMutation(0);
  }
  bool published = false;
  if (!failure.has_value() && write.exclusive) {
    // Inserts never replace a concurrently created file or symbolic link.
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
      failure = FilesystemError({"publish staged file", destination, LastFileError()});
    } else {
      published = true;
    }
  } else if (!failure.has_value()) {
    // Own the recovery directory exclusively, so ReplaceFile never overwrites
    // an unrelated backup. A failed replacement may leave the only original here.
    fs::path recovery = temporary;
    recovery += ".recovery";
    std::error_code error;
    const bool reserved = fs::create_directory(recovery, error);
    if (error || !reserved) {
      if (!error) {
        error = std::make_error_code(std::errc::file_exists);
      }
      failure = FilesystemError({"reserve recovery directory", recovery, error});
    } else if (IsStopRequested(write.mutation.stop_requested)) {
      failure = StoppedMutation(0);
      fs::remove(recovery, error);
      if (error) {
        failure = FilesystemError({"remove empty recovery directory", recovery, error});
      }
    } else {
      const fs::path backup = recovery / "original";
      const FileReplacement replacement{.original = destination,
                                        .replacement = temporary, .backup = backup};
      // Zero flags require metadata/ACL merging to succeed. Unlike MoveFileEx,
      // ReplaceFile preserves creation time, DACLs and existing named streams.
      const auto replace_error = write.mutation.write_effects.replace
          ? write.mutation.write_effects.replace(replacement)
          : (ReplaceFileW(destination.c_str(), temporary.c_str(), backup.c_str(),
                          0, nullptr, nullptr) ? std::error_code{} : LastFileError());
      if (replace_error) {
        EncodableMap details;
        details[EncodableValue("recoveryPath")] = EncodableValue(PathToUtf8(recovery));
        failure = FlutterError("unavailable",
            DescribeFilesystemFailure({"replace staged file", destination, replace_error}) +
            "; recovery directory retained at " + PathToUtf8(recovery), EncodableValue(details));
      } else {
        published = true;
        if (!DeleteFileW(backup.c_str())) {
          failure = WithAffected({FilesystemError({"remove original backup", backup,
                                                   LastFileError()}), 1});
        } else {
          fs::remove(recovery, error);
          if (error) {
            failure = WithAffected({FilesystemError({"remove recovery directory", recovery,
                                                     error}), 1});
          }
        }
        if (failure.has_value()) {
          EncodableMap details = std::get<EncodableMap>(failure->details());
          details[EncodableValue("recoveryPath")] = EncodableValue(PathToUtf8(recovery));
          failure = FlutterError(failure->code(), failure->message(), EncodableValue(details));
        }
      }
    }
  }
  const bool staging_cleanup_needed = failure.has_value() && !published;
  if (staging_cleanup_needed && !DeleteFileW(temporary.c_str())) {
    const auto cleanup_error = LastFileError();
    return FlutterError("unavailable", failure->message() + "; staging cleanup failed: " +
        DescribeFilesystemFailure({"delete staging file", temporary, cleanup_error}),
        failure->details());
  }
  return failure;
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
// @param mutation is the insert request; values.path names the target.
// @returns The affected count and insertedId, or an error.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<EncodableMap> Insert(const FileMutation& mutation) {
  const EncodableMap* values = MapAt(mutation.request, "values");
  if (values == nullptr) {
    return FlutterError("invalid-query", "simple_query: insert requires values");
  }
  const auto path_text = StringAt(*values, "path");
  if (!path_text.has_value() || path_text->empty()) {
    return FlutterError("invalid-query", "simple_query: insert requires values.path");
  }
  auto contents = NewContents(*values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) {
    return *invalid;
  }
  if (IsStopRequested(mutation.stop_requested)) {
    return StoppedMutation(0);
  }
  const fs::path path = PathFromUtf8(*path_text);
  if (BoolAt(*values, "isDirectory") == true) {
    std::error_code error;
    const bool created = fs::create_directories(path, error);
    if (error) {
      return FilesystemError({"create directory", path, error});
    }
    if (!created) {
      return FilesystemError({"create directory", path,
                              std::make_error_code(std::errc::file_exists)});
    }
  } else {
    if (auto failed = CreateParent(path)) {
      return *failed;
    }
    // An insert without bytes or content creates an empty file.
    const auto& text = std::get<std::optional<std::string>>(contents);
    const std::string bytes = text.value_or(std::string());
    if (auto failed = WriteBytes({.path = path, .bytes = bytes,
                                 .exclusive = true, .mutation = mutation})) {
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
  RowsResult listed = ListRecords({.root = mutation.resolve_root(), .media_only = media,
                                   .stop_requested = mutation.stop_requested});
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
    if (IsStopRequested(mutation.stop_requested)) {
      return StoppedMutation(deleted);
    }
    std::error_code error;
    // A target inside a folder deleted earlier is already gone, and
    // removes nothing.
    const auto removed = fs::remove_all(target, error);
    if (error) {
      return WithAffected({FilesystemError({"delete", target, error}), deleted});
    }
    deleted += static_cast<int64_t>(removed);
  }
  return AffectedResult(deleted);
}

// Purpose: Name a cancellable move of one target.
// @param target becomes the new path on success.
// @param values carry the optional newPath.
// @param stop_requested prevents later filesystem steps once stopped.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct TargetMove {
  fs::path* target;
  const EncodableMap& values;
  const StopRequested& stop_requested;
};

// Purpose: Move a target only while the mutation is active.
// @param move names the path, values and stop check.
// @returns Whether the target moved, or an error.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<bool> MoveTarget(const TargetMove& move) {
  auto* target = move.target;
  const auto& values = move.values;
  const auto new_path_text = StringAt(values, "newPath");
  if (!new_path_text.has_value() || new_path_text->empty()) {
    return false;
  }
  const fs::path new_path = PathFromUtf8(*new_path_text);
  if (new_path == *target) {
    return false;
  }
  if (IsStopRequested(move.stop_requested)) {
    return StoppedMutation(0);
  }
  if (auto failed = CreateParent(new_path)) {
    return *failed;
  }
  std::error_code error;
  if (IsStopRequested(move.stop_requested)) {
    return StoppedMutation(0);
  }
  fs::rename(*target, new_path, error);
  if (error) {
    return FilesystemError({"rename", *target, error});
  }
  *target = new_path;
  return true;
}

// Purpose: Name a validated update of one target.
// @param original is the original path.
// @param contents are validated replacement bytes, or absent for a move only.
// @param mutation supplies values, stop checks and write effects.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct TargetUpdate {
  const fs::path& original;
  const std::optional<std::string>& contents;
  const FileMutation& mutation;
};

// Purpose: Apply one validated update while reporting any earlier move on failure.
// @param update names the original path, contents and mutation.
// @returns Whether the target changed, or a failure with confirmed partial effects.
// @throws std::system_error when a path is not valid Unicode.
ErrorOr<bool> UpdateOne(const TargetUpdate& update) {
  const auto& original = update.original;
  const auto& mutation = update.mutation;
  const auto& values = *MapAt(mutation.request, "values");
  if (IsStopRequested(mutation.stop_requested)) {
    return StoppedMutation(0);
  }
  std::error_code error;
  const bool exists = fs::exists(original, error);
  if (error) {
    return FilesystemError({"metadata read", original, error});
  }
  if (!exists) {
    return false;
  }
  fs::path target = original;
  auto moved = MoveTarget({.target = &target, .values = values,
                           .stop_requested = mutation.stop_requested});
  if (moved.has_error()) {
    return moved.error();
  }
  const bool is_directory = fs::is_directory(target, error);
  if (error) {
    return WithAffected({FilesystemError({"metadata read", target, error}),
                         moved.value() ? 1 : 0});
  }
  if (is_directory) {
    return moved.value();
  }
  const auto& text = update.contents;
  if (!text.has_value()) {
    return moved.value();
  }
  if (auto failed = WriteBytes({.path = target, .bytes = *text,
                               .exclusive = false, .mutation = mutation})) {
    const bool unreported_move = moved.value() && ConfirmedEffects(*failed) == 0;
    return WithAffected({*failed, unreported_move ? 1 : 0});
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
  auto contents = NewContents(*values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) {
    return *invalid;
  }
  const auto& text = std::get<std::optional<std::string>>(contents);
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
    if (IsStopRequested(mutation.stop_requested)) {
      return StoppedMutation(updated);
    }
    auto changed = UpdateOne({.original = target, .contents = text, .mutation = mutation});
    if (changed.has_error()) {
      return WithAffected({changed.error(), updated});
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
    // Explicit path/id updates must validate filters just like filtered targets.
    auto filters = ParseFilters(mutation.request);
    if (auto* invalid = std::get_if<std::string>(&filters)) {
      return FlutterError("invalid-query", *invalid);
    }
    if (IsStopRequested(mutation.stop_requested)) {
      return StoppedMutation(0);
    }
    if (type == "insert") {
      return MutationOutcome(Insert(mutation));
    }
    if (type == "delete") {
      return MutationOutcome(Delete(mutation));
    }
    if (type == "update") {
      return MutationOutcome(Update(mutation));
    }
  } catch (const std::system_error& error) {
    // Raised by a path that is not valid Unicode or an unreadable working
    // directory.
    return WithAffected({FlutterError("unavailable",
                        std::string("simple_query: windows filesystem mutation failed - ") +
                            ExceptionText(error)), 0});
  }
  return FlutterError("invalid-query",
                      "simple_query: unknown mutation type " + type.value_or("(none)"));
}

}  // namespace simple_query_windows
