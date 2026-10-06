#include <exception>
#include <fstream>
#include <set>

#include "simple_query_linux_plugin_private.h"

namespace simple_query_linux {

namespace {

/**
 * Purpose: Convert native transport codes to public SimpleQueryErrorCode names.
 * @param code is the kebab-case native error code.
 * @returns The camel-case code name, or null when native code drift is found.
 * @throws Nothing.
 */
std::optional<std::string> PortableErrorCodeName(const std::string& code) {
  if (code == "not-supported") return "notSupported";
  if (code == "permission-denied") return "permissionDenied";
  if (code == "invalid-query") return "invalidQuery";
  if (code == "transient") return "transientFailure";
  if (code == "unavailable") return "unavailable";
  return std::nullopt;
}

/**
 * Purpose: Attach sequential-best-effort metadata to one ordered batch result.
 * @param operation supplies the domain for structured error context.
 * @param result is the mutation value or native failure for this operation.
 * @returns A successful or failed MutationResult-shaped payload.
 * @throws Nothing.
 */
ValuePtr BatchOperationResult(FlValue* operation, ValueResult result) {
  auto payload = result.value != nullptr ? std::move(result.value)
                                         : Value(fl_value_new_map());
  if (result.error.has_value()) {
    MapSetInt(payload.get(), "affectedCount", 0);
  }
  auto metadata = CloneMap(AsMap(FindValue(payload.get(), "metadata")));
  MapSetString(metadata.get(), "batchSemantics", "sequentialBestEffort");
  MapSetString(metadata.get(), "implementation", "native_linux");
  if (result.error.has_value()) {
    auto error = Value(fl_value_new_map());
    const auto portable_code = PortableErrorCodeName(result.error->code);
    MapSetString(error.get(), "code", portable_code.value_or("unavailable"));
    const std::string message =
        portable_code.has_value()
            ? result.error->message
            : result.error->message +
                  " (unrecognized native error code: " + result.error->code +
                  ")";
    MapSetString(error.get(), "message", message);
    MapSetString(error.get(), "domain",
                 StringOr(operation, "domain", "platformSpecific"));
    MapSetString(error.get(), "operation", "write");
    MapSet(metadata.get(), "error", error.release());
  }
  MapSet(payload.get(), "metadata", metadata.release());
  return payload;
}

}  // namespace

namespace {

/** Purpose: Build a portable mutation result with an affected count.
 * @param count is the number of changed records. @param inserted_id is the
 * optional inserted record identifier. @returns A successful result.
 * @throws Nothing. */
ValueResult AffectedMutation(
    int64_t count,
    const std::optional<std::string>& inserted_id = std::nullopt) {
  auto result = Value(fl_value_new_map());
  MapSetInt(result.get(), "affectedCount", count);
  if (inserted_id.has_value()) {
    MapSetString(result.get(), "insertedId", *inserted_id);
  }
  return Success(std::move(result));
}

/** Purpose: Execute one validated filesystem insert.
 * @param values contains the insertion payload. @returns Mutation data or a
 * structured validation/I/O error. @throws Nothing. */
ValueResult InsertMutation(FlValue* values) {
  if (values == nullptr) {
    return Failure("invalid-query", "simple_query: insert requires values");
  }
  const auto path = AsString(FindValue(values, "path"));
  if (!path.has_value() || path->empty()) {
    return Failure("invalid-query",
                   "simple_query: insert requires values.path");
  }
  std::error_code error;
  const std::filesystem::path output_path(*path);
  if (BoolOr(values, "isDirectory", false)) {
    std::filesystem::create_directories(output_path, error);
    if (error) {
      return Failure("unavailable",
                     FileSystemFailure("filesystem directory creation",
                                       output_path, error));
    }
  } else {
    const auto parent_path = output_path.parent_path();
    if (!parent_path.empty()) {
      std::filesystem::create_directories(parent_path, error);
      if (error) {
        return Failure("unavailable",
                       FileSystemFailure("filesystem directory creation",
                                         parent_path, error));
      }
    }
    std::ofstream stream(output_path, std::ios::binary);
    if (!stream.is_open()) {
      return Failure(
          "unavailable",
          "simple_query: could not create output file " + output_path.string());
    }
    stream << AsString(FindValue(values, "content")).value_or("");
    stream.flush();
    if (!stream.good()) {
      return Failure(
          "unavailable",
          "simple_query: could not write output file " + output_path.string());
    }
  }
  return AffectedMutation(1, path);
}

/** Purpose: Build the query used to resolve filtered mutation targets.
 * @param domain is the mutation domain. @param request supplies filters and
 * platform data. @returns A newly owned query map. @throws Nothing. */
ValuePtr MutationQuery(const std::string& domain, FlValue* request) {
  auto query = Value(fl_value_new_map());
  MapSetString(query.get(), "domain", domain);
  FlValue* filters = FindValue(request, "filters");
  MapSet(query.get(), "filters",
         filters != nullptr ? fl_value_ref(filters) : fl_value_new_list());
  if (FlValue* platform_data = FindValue(request, "platformData");
      platform_data != nullptr) {
    MapSet(query.get(), "platformData", fl_value_ref(platform_data));
  }
  return query;
}

/** Purpose: Require an explicit scope before a query-backed deletion.
 * @param request supplies platformData.rootPath.
 * @returns No error for a nonempty root, otherwise invalid-query.
 * @throws Nothing. */
std::optional<NativeError> ValidateDeleteRoot(FlValue* request) {
  FlValue* platform_data = AsMap(FindValue(request, "platformData"));
  const auto root_path = platform_data == nullptr
                             ? std::optional<std::string>()
                             : AsString(FindValue(platform_data, "rootPath"));
  if (root_path.has_value() && !root_path->empty()) return std::nullopt;
  return NativeError{
      "invalid-query",
      "simple_query: delete requires nonempty platformData.rootPath"};
}

/** Purpose: Resolve the filesystem path common to file and media records.
 * @param record is a borrowed queried record.
 * @returns Its canonical path, or null when the record is malformed.
 * @throws Nothing. */
std::optional<std::string> RecordPath(FlValue* record) {
  for (const char* key : {"id", "path", "uriOrPath"}) {
    const auto candidate = AsString(FindValue(record, key));
    if (candidate.has_value() && !candidate->empty()) return candidate;
  }
  return std::nullopt;
}

/** Purpose: Delete every record selected through the native query path.
 * @param host executes target resolution. @param domain is files or media.
 * @param request supplies filters. @returns Mutation data or an I/O error.
 * @throws Nothing. */
ValueResult DeleteMutation(NativeQueryHostApiImpl* host,
                           const std::string& domain, FlValue* request) {
  if (auto error = ValidateDeleteRoot(request); error.has_value()) {
    return ValueResult{ValuePtr(), std::move(error)};
  }
  auto query = MutationQuery(domain, request);
  auto queried = host->Query(query.get());
  if (queried.error.has_value()) return queried;
  FlValue* records = AsList(FindValue(queried.value.get(), "records"));
  int64_t deleted = 0;
  if (records != nullptr) {
    for (size_t index = 0; index < fl_value_get_length(records); index++) {
      FlValue* record = AsMap(fl_value_get_list_value(records, index));
      const auto path = RecordPath(record);
      if (!path.has_value() || path->empty()) continue;
      std::error_code error;
      const auto removed = std::filesystem::remove_all(*path, error);
      if (error) {
        return Failure("unavailable",
                       FileSystemFailure("filesystem delete", *path, error));
      }
      deleted += static_cast<int64_t>(removed);
    }
  }
  return AffectedMutation(deleted);
}

/** Purpose: Preserve path-selection failures before destructive writes begin.
 * @param values owns the ordered, deduplicated canonical target paths.
 * @param error retains a failed target-selection query.
 * @returns Resolved mutation targets or their native error.
 * @throws std::bad_alloc when paths or diagnostics are allocated. */
struct MutationPaths {
  std::set<std::string> values;
  std::optional<NativeError> error;
};

/** Purpose: Resolve explicit or filtered paths for an update.
 * @param host executes filtered queries. @param domain is files or media.
 * @param request supplies filters. @param values supplies explicit IDs.
 * @returns Ordered paths or a query error. @throws Nothing. */
MutationPaths ResolveMutationPaths(NativeQueryHostApiImpl* host,
                                   const std::string& domain, FlValue* request,
                                   FlValue* values) {
  MutationPaths result;
  const auto path = AsString(FindValue(values, "path"));
  const auto id = AsString(FindValue(values, "id"));
  if (path.has_value() && !path->empty()) {
    result.values.insert(*path);
  } else if (id.has_value() && !id->empty()) {
    result.values.insert(*id);
  }
  if (!result.values.empty()) return result;
  auto query = MutationQuery(domain, request);
  auto queried = host->Query(query.get());
  if (queried.error.has_value()) {
    result.error = queried.error;
    return result;
  }
  FlValue* records = AsList(FindValue(queried.value.get(), "records"));
  if (records == nullptr) return result;
  for (size_t index = 0; index < fl_value_get_length(records); index++) {
    FlValue* record = AsMap(fl_value_get_list_value(records, index));
    const auto record_path = RecordPath(record);
    if (record_path.has_value() && !record_path->empty()) {
      result.values.insert(*record_path);
    }
  }
  return result;
}

/** Purpose: Track validation and rename outcomes before content replacement.
 * @param value holds the effective target path after any rename.
 * @param exists and changed distinguish missing targets from completed renames.
 * @param error retains filesystem validation or rename failure.
 * @returns An owned prepared target for subsequent content mutation.
 * @throws std::bad_alloc when paths or diagnostics are allocated. */
struct PreparedPath {
  std::filesystem::path value;
  bool exists = true;
  bool changed = false;
  std::optional<NativeError> error;
};

/** Purpose: Validate and optionally rename one update target.
 * @param original_path identifies the existing target. @param values supplies
 * newPath. @returns Effective path, change state, or I/O error. @throws
 * Nothing.
 */
PreparedPath PrepareUpdatePath(const std::string& original_path,
                               FlValue* values) {
  PreparedPath result{std::filesystem::path(original_path), true, false,
                      std::nullopt};
  std::error_code error;
  result.exists = std::filesystem::exists(result.value, error);
  if (error) {
    result.error = NativeError{
        "unavailable",
        FileSystemFailure("filesystem existence check", result.value, error)};
    return result;
  }
  if (!result.exists) return result;
  const auto new_path = AsString(FindValue(values, "newPath"));
  if (!new_path.has_value() || new_path->empty() ||
      *new_path == original_path) {
    return result;
  }
  const std::filesystem::path next_path(*new_path);
  const auto parent_path = next_path.parent_path();
  if (!parent_path.empty()) {
    std::filesystem::create_directories(parent_path, error);
    if (error) {
      result.error = NativeError{
          "unavailable", FileSystemFailure("filesystem directory creation",
                                           parent_path, error)};
      return result;
    }
  }
  std::filesystem::rename(result.value, next_path, error);
  if (error) {
    result.error = NativeError{
        "unavailable",
        FileSystemFailure("filesystem rename", result.value, error)};
    return result;
  }
  result.value = next_path;
  result.changed = true;
  return result;
}

/** Purpose: Carry one content-write outcome without throwing filesystem errors.
 * @param changed distinguishes a completed write from an untouched target.
 * @param error preserves invalid content or filesystem failure.
 * @returns An explicit write outcome for affected-record accounting.
 * @throws std::bad_alloc when an error diagnostic is allocated. */
struct ContentUpdate {
  bool changed = false;
  std::optional<NativeError> error;
};

/** Purpose: Apply bytes or text content to one non-directory target.
 * @param path identifies the effective target. @param values supplies content.
 * @returns Change state or a validation/I/O error. @throws Nothing. */
ContentUpdate WriteUpdateContent(const std::filesystem::path& path,
                                 FlValue* values) {
  ContentUpdate result;
  std::error_code error;
  const bool is_directory = std::filesystem::is_directory(path, error);
  if (error) {
    result.error = NativeError{
        "unavailable", FileSystemFailure("filesystem metadata", path, error)};
    return result;
  }
  if (is_directory) return result;
  FlValue* bytes = FindValue(values, "bytes");
  FlValue* content = FindValue(values, "content");
  if (bytes == nullptr && content == nullptr) return result;
  if (bytes != nullptr &&
      fl_value_get_type(bytes) != FL_VALUE_TYPE_UINT8_LIST) {
    result.error = NativeError{
        "invalid-query",
        "simple_query: update expects values.bytes as Uint8List when provided"};
    return result;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    result.error = NativeError{"unavailable",
                               "simple_query: could not open file for update"};
    return result;
  }
  if (bytes != nullptr) {
    stream.write(reinterpret_cast<const char*>(fl_value_get_uint8_list(bytes)),
                 static_cast<std::streamsize>(fl_value_get_length(bytes)));
  } else {
    stream << AsString(content).value_or("");
  }
  stream.flush();
  if (!stream.good()) {
    result.error = NativeError{
        "unavailable", "simple_query: could not write file " + path.string()};
    return result;
  }
  result.changed = true;
  return result;
}

/** Purpose: Update every explicitly or query-selected filesystem target.
 * @param host executes target queries. @param domain is files or media.
 * @param request supplies filters. @param values supplies update fields.
 * @returns Mutation data or a structured validation/I/O error. @throws Nothing.
 */
ValueResult UpdateMutation(NativeQueryHostApiImpl* host,
                           const std::string& domain, FlValue* request,
                           FlValue* values) {
  if (values == nullptr) {
    return Failure("invalid-query", "simple_query: update requires values");
  }
  auto paths = ResolveMutationPaths(host, domain, request, values);
  if (paths.error.has_value()) {
    return ValueResult{ValuePtr(), paths.error};
  }
  int64_t updated = 0;
  for (const auto& original_path : paths.values) {
    auto prepared = PrepareUpdatePath(original_path, values);
    if (prepared.error.has_value()) {
      return ValueResult{ValuePtr(), prepared.error};
    }
    if (!prepared.exists) continue;
    auto content = WriteUpdateContent(prepared.value, values);
    if (content.error.has_value()) {
      return ValueResult{ValuePtr(), content.error};
    }
    if (prepared.changed || content.changed) updated += 1;
  }
  return AffectedMutation(updated);
}

}  // namespace

/** Purpose: Execute one filesystem mutation with fail-closed I/O handling.
 * @param request is the decoded Pigeon mutation payload. @returns A mutation
 * result or structured native error. @throws Native allocation exceptions for
 * the outer callback boundary to translate. */
ValueResult NativeQueryHostApiImpl::Mutate(FlValue* request) {
  const std::string domain = StringOr(request, "domain", "platformSpecific");
  if (domain != "files" && domain != "media") {
    return Failure("not-supported",
                   "simple_query: mutate is not supported for domain " +
                       domain + " on Linux host");
  }
  const std::string type = StringOr(request, "type", "");
  if (type == "insert") {
    return InsertMutation(AsMap(FindValue(request, "values")));
  }
  if (type == "delete") {
    return DeleteMutation(this, domain, request);
  }
  if (type == "update") {
    return UpdateMutation(this, domain, request,
                          AsMap(FindValue(request, "values")));
  }
  return Failure("invalid-query",
                 "simple_query: unknown mutation type " + type);
}

/** Purpose: Execute every well-formed mutation in stable input order.
 * @param request is the decoded batch payload. @returns One annotated result
 * per operation, preserving failures. @throws Native allocation exceptions
 * before or after per-operation containment. */
ValueResult NativeQueryHostApiImpl::Batch(FlValue* request) {
  FlValue* operations = AsList(FindValue(request, "operations"));
  if (operations == nullptr) {
    return Failure("invalid-query", "simple_query: batch requires operations");
  }

  for (size_t index = 0; index < fl_value_get_length(operations); index++) {
    if (AsMap(fl_value_get_list_value(operations, index)) == nullptr) {
      return Failure("invalid-query",
                     "simple_query: batch operation must be a map");
    }
  }

  auto results = Value(fl_value_new_list());
  for (size_t index = 0; index < fl_value_get_length(operations); index++) {
    FlValue* operation = AsMap(fl_value_get_list_value(operations, index));
    auto merged = CloneMap(operation);
    if (FindValue(operation, "platformData") == nullptr &&
        FindValue(request, "platformData") != nullptr) {
      MapSet(merged.get(), "platformData",
             fl_value_ref(FindValue(request, "platformData")));
    }

    ValueResult result;
    try {
      result = Mutate(merged.get());
    } catch (const std::exception& error) {
      result = Failure("unavailable",
                       "simple_query: Linux batch operation failed - " +
                           std::string(error.what()));
    } catch (...) {
      result =
          Failure("unavailable", "simple_query: Linux batch operation failed");
    }
    fl_value_append_take(
        results.get(),
        BatchOperationResult(operation, std::move(result)).release());
  }

  auto response = Value(fl_value_new_map());
  MapSet(response.get(), "results", results.release());
  return Success(std::move(response));
}

}  // namespace simple_query_linux
