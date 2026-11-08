#include "file_mutations.h"

#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "boundary_formats.h"
#include "file_records.h"
#include "file_mutation_errors.h"
#include "file_write.h"
#include "query_shaping.h"
#include "request_values.h"
#include "owned_message_error.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
using file_mutation_detail::CheckMutationStop;
using file_mutation_detail::ConfirmedEffects;
using file_mutation_detail::FilesystemError;
using file_mutation_detail::FileWrite;
using file_mutation_detail::WithAffected;
using file_mutation_detail::WriteBytes;
namespace fs = std::filesystem;

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

// Purpose: Exclusively create a requested directory tree.
// @param path names the inserted directory.
// @returns No failure, a creation error or an existing-destination error.
// @throws std::exception when diagnostics or allocation fail.
std::optional<FlutterError> InsertDirectory(const fs::path& path) {
  std::error_code error;
  const bool created = fs::create_directories(path, error);
  if (error) return FilesystemError({"create directory", path, error});
  if (!created) {
    return FilesystemError({"create directory", path,
                            std::make_error_code(std::errc::file_exists)});
  }
  return std::nullopt;
}

// Purpose: Create one requested file or directory from validated contents.
// @param write names the new path, contents and mutation policy.
// @returns No failure or a parent/creation/publication error.
// @throws std::exception from effects or diagnostics after scope-owned cleanup.
std::optional<FlutterError> InsertEntry(const FileWrite& write) {
  const auto& values = *MapAt(write.mutation.request, "values");
  if (BoolAt(values, "isDirectory") == true) return InsertDirectory(write.path);
  if (auto failed = CreateParent(write.path)) return failed;
  return WriteBytes(write);
}

// Purpose: Name an insert whose path text and contents have already been validated.
// @param mutation supplies effects and cancellation.
// @param path_text is the required target path.
// @param contents are validated bytes or absent for an empty-file insert.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PreparedInsert {
  const FileMutation& mutation;
  const std::string& path_text;
  const std::optional<std::string>& contents;
};

// Purpose: Execute one validated insert and construct its successful result.
// @param insert names the validated path, contents and mutation.
// @returns An inserted result or a stop/filesystem failure.
// @throws std::exception after scope-owned cleanup.
ErrorOr<EncodableMap> InsertPrepared(const PreparedInsert& insert) {
  if (auto stopped = CheckMutationStop(insert.mutation.stop_requested)) return *stopped;
  const fs::path path = PathFromUtf8(insert.path_text);
  const std::string bytes = insert.contents.value_or(std::string());
  if (auto failure = InsertEntry({.path = path, .bytes = bytes, .exclusive = true,
                                   .mutation = insert.mutation})) return *failure;
  EncodableMap result = AffectedResult(1);
  result[EncodableValue("insertedId")] = EncodableValue(insert.path_text);
  return result;
}

// Purpose: Validate required insert values before any filesystem effect.
// @param mutation is the insert request.
// @returns The inserted result or a validation/filesystem failure.
// @throws std::exception after scope-owned cleanup.
ErrorOr<EncodableMap> Insert(const FileMutation& mutation) {
  const EncodableMap* values = MapAt(mutation.request, "values");
  if (values == nullptr) return FlutterError("invalid-query", "simple_query: insert requires values");
  const auto path = StringAt(*values, "path");
  if (!path.has_value() || path->empty()) {
    return FlutterError("invalid-query", "simple_query: insert requires values.path");
  }
  auto contents = NewContents(*values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) return *invalid;
  return InsertPrepared({.mutation = mutation, .path_text = *path,
                          .contents = std::get<std::optional<std::string>>(contents)});
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
    if (auto stopped = CheckMutationStop(mutation.stop_requested, deleted)) return *stopped;
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
  if (auto stopped = CheckMutationStop(move.stop_requested)) return *stopped;
  if (auto failed = CreateParent(new_path)) {
    return *failed;
  }
  std::error_code error;
  if (auto stopped = CheckMutationStop(move.stop_requested)) return *stopped;
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

// Purpose: Describe content processing after a target has optionally moved.
// @param target is the current content path.
// @param update carries validated contents and mutation effects.
// @param moved records an already completed move of this same target.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct TargetContents {
  const fs::path& target;
  const TargetUpdate& update;
  bool moved;
};

// Purpose: Finish one content update without counting the same moved target twice.
// @param contents names the current path and preceding move.
// @returns Whether the target changed, or an error disclosing confirmed effects.
// @throws std::exception from effects/diagnostics after scope-owned staging cleanup.
ErrorOr<bool> FinishTargetUpdate(const TargetContents& contents) {
  std::error_code error;
  const bool directory = fs::is_directory(contents.target, error);
  if (error) {
    return WithAffected({FilesystemError({"metadata read", contents.target, error}),
                         contents.moved ? 1 : 0});
  }
  if (directory || !contents.update.contents.has_value()) return contents.moved;
  if (auto failed = WriteBytes({.path = contents.target, .bytes = *contents.update.contents,
                               .exclusive = false, .mutation = contents.update.mutation})) {
    const bool unreported_move = contents.moved && ConfirmedEffects(*failed) == 0;
    return WithAffected({*failed, unreported_move ? 1 : 0});
  }
  return true;
}

// Purpose: Check existence and move one target before processing its validated contents.
// @param update names the original path, contents and mutation.
// @returns Whether the target changed, false if missing, or a failure.
// @throws std::exception from effects/diagnostics after scope-owned staging cleanup.
ErrorOr<bool> UpdateOne(const TargetUpdate& update) {
  const auto& mutation = update.mutation;
  if (auto stopped = CheckMutationStop(mutation.stop_requested)) return *stopped;
  std::error_code error;
  const bool exists = fs::exists(update.original, error);
  if (error) return FilesystemError({"metadata read", update.original, error});
  if (!exists) return false;
  fs::path target = update.original;
  auto moved = MoveTarget({.target = &target, .values = *MapAt(mutation.request, "values"),
                           .stop_requested = mutation.stop_requested});
  if (moved.has_error()) return moved.error();
  return FinishTargetUpdate({.target = target, .update = update, .moved = moved.value()});
}

// Purpose: Resolve explicit path/id updates or the rows their validated filters match.
// @param mutation names the validated update request and search root.
// @returns Target paths or a discovery failure.
// @throws std::exception when path conversion or allocation fails.
ErrorOr<std::vector<fs::path>> UpdateTargets(const FileMutation& mutation) {
  const auto& values = *MapAt(mutation.request, "values");
  const auto path = StringAt(values, "path");
  if (path.has_value() && !path->empty()) return std::vector<fs::path>{PathFromUtf8(*path)};
  const auto id = StringAt(values, "id");
  if (id.has_value() && !id->empty()) return std::vector<fs::path>{PathFromUtf8(*id)};
  return FilteredTargets(mutation);
}

// Purpose: Name the validated targets and contents of one update.
// @param mutation supplies request values, effects and cancellation.
// @param targets names the selected paths.
// @param contents are validated replacement bytes or absent for moves only.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct SelectedUpdate {
  const FileMutation& mutation;
  const std::vector<fs::path>& targets;
  const std::optional<std::string>& contents;
};

// Purpose: Apply selected updates until failure or cancellation, reporting confirmed effects.
// @param update names the validated targets, contents and mutation.
// @returns The affected result or the first failure with prior confirmed effects.
// @throws std::exception after scope-owned cleanup.
ErrorOr<EncodableMap> UpdateSelected(const SelectedUpdate& update) {
  int64_t updated = 0;
  for (const auto& target : update.targets) {
    if (auto stopped = CheckMutationStop(update.mutation.stop_requested, updated)) return *stopped;
    auto changed = UpdateOne({.original = target, .contents = update.contents,
                              .mutation = update.mutation});
    if (changed.has_error()) return WithAffected({changed.error(), updated});
    if (changed.value()) ++updated;
  }
  return AffectedResult(updated);
}

// Purpose: Validate update values and select targets before applying any changes.
// @param mutation names the request, its domain and its root.
// @returns The affected result or a validation/discovery/filesystem failure.
// @throws std::exception after scope-owned cleanup.
ErrorOr<EncodableMap> Update(const FileMutation& mutation) {
  const EncodableMap* values = MapAt(mutation.request, "values");
  if (values == nullptr) return FlutterError("invalid-query", "simple_query: update requires values");
  auto contents = NewContents(*values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) return *invalid;
  auto targets = UpdateTargets(mutation);
  if (targets.has_error()) return targets.error();
  return UpdateSelected({.mutation = mutation, .targets = targets.value(),
                          .contents = std::get<std::optional<std::string>>(contents)});
}

// Purpose: Validate filters and cancellation before selecting any mutation target.
// @param mutation names the decoded request and stop check.
// @returns No failure, an invalid-filter error or a stopped error.
// @throws std::exception from cancellation or allocation before mutation effects.
std::optional<FlutterError> ValidateMutation(const FileMutation& mutation) {
  auto filters = ParseFilters(mutation.request);
  if (auto* invalid = std::get_if<std::string>(&filters)) return FlutterError("invalid-query", *invalid);
  return CheckMutationStop(mutation.stop_requested);
}

// Purpose: Dispatch one validated mutation and attach partial-effect details to failures.
// @param mutation names the decoded request and filesystem effects.
// @param type is the validated request's wire operation, or absent.
// @returns The operation outcome or an unknown-type error.
// @throws std::exception after scope-owned cleanup.
ErrorOr<EncodableMap> DispatchMutation(const FileMutation& mutation,
                                      const std::optional<std::string>& type) {
  if (type == "insert") return MutationOutcome(Insert(mutation));
  if (type == "delete") return MutationOutcome(Delete(mutation));
  if (type == "update") return MutationOutcome(Update(mutation));
  return FlutterError("invalid-query",
                      "simple_query: unknown mutation type " + type.value_or("(none)"));
}

// Purpose: Describe a thrown filesystem diagnostic without asserting rollback.
// @param error is the original filesystem exception.
// @returns An unavailable error with a confirmed lower bound and partial-effects flag.
// @throws std::exception when diagnostic conversion or allocation fails.
FlutterError MutationException(const std::system_error& error) {
  return WithAffected({FlutterError("unavailable",
      std::string("simple_query: windows filesystem mutation failed - ") + ExceptionText(error)), 0});
}

}  // namespace

// Purpose: Insert, update or delete files or media. A filtered delete or
// update targets the rows a query with the same filters returns: their path
// for files, and their uriOrPath for media.
// @param mutation names the request, its domain and its root.
// @returns The affected count, plus insertedId for an insert;
// invalid-query for a malformed request; unavailable when a filesystem step
// fails, naming the step and the path.
// @throws std::exception from injected callbacks after scope-owned cleanup;
// filesystem failures are returned as errors.
ErrorOr<EncodableMap> MutateFiles(const FileMutation& mutation) {
  const auto type = StringAt(mutation.request, "type");
  try {
    if (auto failure = ValidateMutation(mutation)) return *failure;
    return DispatchMutation(mutation, type);
  } catch (const std::system_error& error) {
    return MutationException(error);
  }
}

}  // namespace simple_query_windows
