#include "file_mutations.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "boundary_formats.h"
#include "file_records.h"
#include "query_shaping.h"
#include "request_values.h"
#include "owned_message_error.h"
#include "structured_log.h"

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
std::error_code LastFileError() noexcept {
  return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

// Purpose: Adapt logical paths to Win32 extended syntax without changing public paths.
// @param path names a native filesystem operation, possibly beyond MAX_PATH.
// @returns An absolute extended drive/UNC path on Windows, or the original in portable tests.
// @throws std::exception when absolute-path conversion or allocation fails.
fs::path NativeFilePath(const fs::path& path) {
#if defined(_WIN32)
  const auto& given = path.native();
  if (given.rfind(L"\\\\?\\", 0) == 0) return path;
  const std::wstring native = fs::absolute(path).lexically_normal().make_preferred().native();
  const bool unc = native.rfind(L"\\\\", 0) == 0;
  return fs::path(unc ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native);
#else
  return path;
#endif
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

// Purpose: Prepare a cleanup log before acquiring resources, avoiding allocation while unwinding.
// @param path identifies the owned resource.
// @returns A JSON prefix ready for an allocation-free cleanup outcome.
// @throws std::exception when path conversion or log allocation fails.
std::string CleanupLogPrefix(const fs::path& path) {
  auto line = FormatLogRecord({LogLevel::kError, "file_mutation_cleanup",
                              {{"path", PathToUtf8(path)}}});
  line.pop_back();
  return line;
}

// Purpose: Describe a cleanup result without allocating during destruction.
// @param prefix is the prepared JSON prefix identifying the resource.
// @param step is a fixed operation name, including intentional recovery retention.
// @param error is the native error, or zero for intentional retention.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct CleanupRecord {
  const std::string& prefix;
  const char* step;
  DWORD error;
};

// Purpose: Record cleanup outcomes with prepared text and non-throwing C output.
// @param record names the resource, action and native result.
// @returns Nothing.
// @throws Nothing; C standard-error output cannot throw C++ exceptions.
void RecordCleanup(const CleanupRecord& record) noexcept {
  std::fprintf(stderr, "%s,\"step\":\"%s\",\"win32Error\":%lu}\n",
               record.prefix.c_str(), record.step, static_cast<unsigned long>(record.error));
}

// Purpose: Own a staging handle and file until publication, including exception unwinding.
// @param none; Create prepares the path before acquiring the native handle.
// @returns Nothing; this is a class.
// @throws Nothing; resource preparation documents its allocation failures.
class StagingFile {
 public:
  // Purpose: Start with no owned native resources.
  // @param none.
  // @returns Nothing.
  // @throws Nothing.
  StagingFile() = default;

  // Purpose: Close and discard staging on every exit, recording cleanup failures.
  // @param none.
  // @returns Nothing.
  // @throws Nothing; the original destination is never deleted here.
  ~StagingFile() noexcept {
    const auto error = Discard();
    if (error) RecordCleanup({log_, "discard_staging", static_cast<DWORD>(error.value())});
  }

  StagingFile(const StagingFile&) = delete;
  StagingFile& operator=(const StagingFile&) = delete;

  // Purpose: Prepare owned path/log storage, then exclusively create a staging file.
  // @param path is moved into ownership before CreateFileW is called.
  // @returns The creation error, or no error with ownership already established.
  // @throws std::exception during preparation, before acquiring the handle.
  std::error_code Create(fs::path path) {
    path_ = std::move(path);
    native_path_ = NativeFilePath(path_);
    log_ = CleanupLogPrefix(path_);
    handle_ = CreateFileW(native_path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    owns_path_ = handle_ != INVALID_HANDLE_VALUE;
    return owns_path_ ? std::error_code{} : LastFileError();
  }

  // Purpose: Give the native write handle borrowed by one stage operation.
  // @param none.
  // @returns The owned handle, or INVALID_HANDLE_VALUE after close.
  // @throws Nothing.
  HANDLE handle() const noexcept { return handle_; }

  // Purpose: Give the staging path while its owner remains alive.
  // @param none.
  // @returns The borrowed path.
  // @throws Nothing.
  const fs::path& path() const noexcept { return path_; }

  // Purpose: Close the native handle, retaining ownership if close fails.
  // @param none.
  // @returns The close error, or no error if already closed or closed now.
  // @throws Nothing.
  std::error_code Close() noexcept {
    if (handle_ == INVALID_HANDLE_VALUE) return {};
    if (!CloseHandle(handle_)) return LastFileError();
    handle_ = INVALID_HANDLE_VALUE;
    return {};
  }

  // Purpose: Close and remove only the owned staging file.
  // @param none.
  // @returns The cleanup error, or no error if nothing remains owned.
  // @throws Nothing.
  std::error_code Discard() noexcept {
    if (const auto error = Close()) return error;
    if (!owns_path_) return {};
    if (!DeleteFileW(native_path_.c_str())) return LastFileError();
    owns_path_ = false;
    return {};
  }

  // Purpose: Relinquish the staging name immediately after successful publication.
  // @param none.
  // @returns Nothing.
  // @throws Nothing.
  void Published() noexcept { owns_path_ = false; }

  // Purpose: Borrow the prepared API path without allocating after acquisition.
  // @param none.
  // @returns The extended Win32 staging path.
  // @throws Nothing.
  const fs::path& native_path() const noexcept { return native_path_; }

 private:
  fs::path path_;
  fs::path native_path_;
  std::string log_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool owns_path_ = false;
};

// Purpose: Own an empty recovery directory, retaining it before originals can enter it.
// @param none; its constructor receives its prepared path.
// @returns Nothing; this is a class.
// @throws Nothing; its constructor documents preparation failures.
class RecoveryDirectory {
 public:
  // Purpose: Prepare path and cleanup logging before creating the directory.
  // @param path names a short sibling of the content destination.
  // @returns Nothing.
  // @throws std::exception during preparation, before native resource acquisition.
  explicit RecoveryDirectory(fs::path path)
      : path_(std::move(path)), native_path_(NativeFilePath(path_)),
        log_(CleanupLogPrefix(path_)) {}

  // Purpose: Remove only empty unused directories; retained originals are never deleted.
  // @param none.
  // @returns Nothing.
  // @throws Nothing; failures and intentional retention are recorded without allocation.
  ~RecoveryDirectory() noexcept {
    if (!owned_) return;
    if (retained_) {
      RecordCleanup({log_, "retain_recovery", 0});
      return;
    }
    const auto error = RemoveEmpty();
    if (error) RecordCleanup({log_, "remove_empty_recovery", static_cast<DWORD>(error.value())});
  }

  RecoveryDirectory(const RecoveryDirectory&) = delete;
  RecoveryDirectory& operator=(const RecoveryDirectory&) = delete;

  // Purpose: Exclusively reserve the recovery directory with immediate ownership.
  // @param none.
  // @returns The creation error, or no error after ownership is established.
  // @throws Nothing.
  std::error_code Reserve() noexcept {
    owned_ = CreateDirectoryW(native_path_.c_str(), nullptr) != 0;
    return owned_ ? std::error_code{} : LastFileError();
  }

  // Purpose: Preserve the directory before any replacement may move the original into it.
  // @param none.
  // @returns Nothing.
  // @throws Nothing.
  void Retain() noexcept { retained_ = true; }

  // Purpose: Give the recovery directory for diagnostics and its backup path.
  // @param none.
  // @returns The borrowed path.
  // @throws Nothing.
  const fs::path& path() const noexcept { return path_; }

  // Purpose: Remove a known-empty recovery directory without touching its contents.
  // @param none.
  // @returns The removal error, or no error if no directory remains owned.
  // @throws Nothing.
  std::error_code RemoveEmpty() noexcept {
    if (!owned_) return {};
    if (!RemoveDirectoryW(native_path_.c_str())) return LastFileError();
    owned_ = false;
    return {};
  }

 private:
  fs::path path_;
  fs::path native_path_;
  std::string log_;
  bool owned_ = false;
  bool retained_ = false;
};

// Purpose: Generate a bounded sibling component independent of the destination filename.
// @param destination names the content destination whose parent is used.
// @param suffix identifies a stage or recovery directory.
// @returns A same-directory path with a process/counter-specific short component.
// @throws std::exception when path or string allocation fails.
fs::path ShortSibling(const fs::path& destination, const char* suffix) {
  static std::atomic<uint64_t> sequence{0};
  return destination.parent_path() /
      (".simple-query-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(sequence.fetch_add(1)) + suffix);
}

// Purpose: Resolve content updates through symbolic links while leaving inserts exclusive.
// @param write names the destination and publication policy.
// @returns The content destination, or its resolution failure.
// @throws std::exception when path conversion or allocation fails.
std::variant<fs::path, FlutterError> ContentDestination(const FileWrite& write) {
  if (write.exclusive) return write.path;
  std::error_code error;
  auto path = fs::canonical(write.path, error);
  if (error) return FilesystemError({"resolve update target", write.path, error});
  return path;
}

// Purpose: Name cancellable stage creation.
// @param destination supplies the parent for short staging names.
// @param stage owns every successful creation immediately.
// @param stop_requested prevents later acquisition when stopped.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct StageCreation {
  const fs::path& destination;
  StagingFile& stage;
  const StopRequested& stop_requested;
};

// Purpose: Exclusively create staging, retrying only collisions with existing names.
// @param creation names the destination, owner and stop check.
// @returns No failure after acquisition, or a stop/creation failure.
// @throws std::exception during preparation; acquired resources remain scope-owned.
std::optional<FlutterError> CreateStage(const StageCreation& creation) {
  while (true) {
    if (IsStopRequested(creation.stop_requested)) return StoppedMutation(0);
    const auto error = creation.stage.Create(ShortSibling(creation.destination, ".tmp"));
    if (!error) return std::nullopt;
    if (error.value() != static_cast<int>(ERROR_FILE_EXISTS)) {
      return FilesystemError({"create staging file", creation.stage.path(), error});
    }
  }
}

// Purpose: Name one owned staging operation and its content request.
// @param stage owns the open handle and staging file.
// @param write supplies contents, cancellation and injected boundaries.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct StageOperation {
  StagingFile& stage;
  const FileWrite& write;
};

// Purpose: Write a complete stage without releasing its exception-safe ownership.
// @param operation supplies the owner, bytes and effects.
// @returns No failure, a stopped error or a native write error.
// @throws std::exception from effects or allocation; staging remains scope-owned.
std::optional<FlutterError> WriteStage(const StageOperation& operation) {
  const auto& write = operation.write;
  if (IsStopRequested(write.mutation.stop_requested)) return StoppedMutation(0);
  const auto error = write.mutation.write_effects.write
      ? write.mutation.write_effects.write(operation.stage.handle(), write.bytes)
      : WriteAll({.handle = operation.stage.handle(), .bytes = write.bytes,
                  .stop_requested = write.mutation.stop_requested});
  if (error == std::errc::operation_canceled) return StoppedMutation(0);
  if (error) return FilesystemError({"write staging file", operation.stage.path(), error});
  return std::nullopt;
}

// Purpose: Flush and close a complete stage before publishing its path.
// @param operation supplies the owner, stop check and flush effect.
// @returns No failure, a stopped error or a native flush/close error.
// @throws std::exception from effects or allocation; staging remains scope-owned.
std::optional<FlutterError> FlushStage(const StageOperation& operation) {
  const auto& mutation = operation.write.mutation;
  if (IsStopRequested(mutation.stop_requested)) return StoppedMutation(0);
  const auto error = mutation.write_effects.flush
      ? mutation.write_effects.flush(operation.stage.handle())
      : (FlushFileBuffers(operation.stage.handle()) ? std::error_code{} : LastFileError());
  if (error) return FilesystemError({"flush staging file", operation.stage.path(), error});
  if (const auto close = operation.stage.Close()) {
    return FilesystemError({"close staging file", operation.stage.path(), close});
  }
  return std::nullopt;
}

// Purpose: Describe a failed publication with preserved recovery contents.
// @param error is the original operation failure.
// @param recovery identifies a retained directory that may hold the only original.
// @param affected is the confirmed number of targets already published.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct RecoveryFailure {
  const FlutterError& error;
  const fs::path& recovery;
  int64_t affected;
};

// Purpose: Disclose retained recovery data and confirmed partial effects.
// @param failure names the error, retained location and affected count.
// @returns An unavailable error with recoveryPath and effect details.
// @throws std::exception from diagnostics or allocation; recovery remains retained.
FlutterError RecoveryError(const RecoveryFailure& failure) {
  const auto counted = WithAffected({failure.error, failure.affected});
  EncodableMap details = std::get<EncodableMap>(counted.details());
  details[EncodableValue("recoveryPath")] = EncodableValue(PathToUtf8(failure.recovery));
  return FlutterError(counted.code(), counted.message() +
      "; recovery directory retained at " + PathToUtf8(failure.recovery), EncodableValue(details));
}

// Purpose: Name cleanup after a replacement has published successfully.
// @param recovery owns the retained recovery directory.
// @param backup holds the original until its explicit deletion succeeds.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct ReplacementCleanup {
  RecoveryDirectory& recovery;
  const fs::path& backup;
};

// Purpose: Remove a superseded original only after successful publication.
// @param cleanup names the recovery owner and original backup.
// @returns No failure, or a disclosed recovery error with affectedCount=1.
// @throws std::exception from diagnostics; the recovery owner never deletes originals.
std::optional<FlutterError> CleanupReplacement(const ReplacementCleanup& cleanup) {
  const auto native_backup = NativeFilePath(cleanup.backup);
  if (!DeleteFileW(native_backup.c_str())) {
    return RecoveryError({FilesystemError({"remove original backup", cleanup.backup,
                                           LastFileError()}), cleanup.recovery.path(), 1});
  }
  if (const auto error = cleanup.recovery.RemoveEmpty()) {
    return RecoveryError({FilesystemError({"remove recovery directory", cleanup.recovery.path(),
                                           error}), cleanup.recovery.path(), 1});
  }
  return std::nullopt;
}

// Purpose: Name publication of a complete stage.
// @param stage owns the staged path until publication succeeds.
// @param destination is the canonical update path or exclusive insert path.
// @param write supplies the policy, cancellation and replacement boundary.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FilePublication {
  StagingFile& stage;
  const fs::path& destination;
  const FileWrite& write;
};

// Purpose: Replace an existing file while preserving metadata and recoverable originals.
// @param publication names the complete stage, destination and request.
// @returns No failure, a stop, a reservation error or a disclosed recovery error.
// @throws std::exception from effects/diagnostics; originals remain scope-retained.
std::optional<FlutterError> PublishReplacement(const FilePublication& publication) {
  RecoveryDirectory recovery(ShortSibling(publication.destination, ".recovery"));
  const fs::path backup = recovery.path() / "original";
  const auto native_destination = NativeFilePath(publication.destination);
  const auto native_backup = NativeFilePath(backup);
  if (const auto error = recovery.Reserve()) {
    return FilesystemError({"reserve recovery directory", recovery.path(), error});
  }
  if (IsStopRequested(publication.write.mutation.stop_requested)) return StoppedMutation(0);
  const FileReplacement replacement{.original = native_destination,
      .replacement = publication.stage.native_path(), .backup = native_backup};
  recovery.Retain();
  const auto& effects = publication.write.mutation.write_effects;
  const auto error = effects.replace ? effects.replace(replacement)
      : (ReplaceFileW(replacement.original.c_str(), replacement.replacement.c_str(),
                      replacement.backup.c_str(), 0, nullptr, nullptr)
             ? std::error_code{} : LastFileError());
  if (error) {
    return RecoveryError({FilesystemError({"replace staged file", publication.destination,
                                           error}), recovery.path(), 0});
  }
  publication.stage.Published();
  return CleanupReplacement({recovery, backup});
}

// Purpose: Publish exclusively for insert or preserve metadata during replacement.
// @param publication names the owned stage and request.
// @returns No failure or a stop/native/recovery error.
// @throws std::exception from effects or diagnostics; resources remain scope-owned.
std::optional<FlutterError> PublishStage(const FilePublication& publication) {
  if (IsStopRequested(publication.write.mutation.stop_requested)) return StoppedMutation(0);
  if (!publication.write.exclusive) return PublishReplacement(publication);
  const auto native_destination = NativeFilePath(publication.destination);
  if (!MoveFileExW(publication.stage.native_path().c_str(), native_destination.c_str(),
                   MOVEFILE_WRITE_THROUGH)) {
    return FilesystemError({"publish staged file", publication.destination, LastFileError()});
  }
  publication.stage.Published();
  return std::nullopt;
}

// Purpose: Name explicit staging cleanup and the outcome it must preserve.
// @param stage owns any unpublished staging data.
// @param failure is the earlier failure, if any.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct StageCompletion {
  StagingFile& stage;
  std::optional<FlutterError> failure;
};

// Purpose: Close/discard unpublished staging and retain the original failure details.
// @param completion names the stage and earlier result.
// @returns The earlier result, or a staging-cleanup error with its details preserved.
// @throws std::exception from diagnostics; the staging destructor retries cleanup.
std::optional<FlutterError> FinishStage(const StageCompletion& completion) {
  const auto error = completion.stage.Discard();
  if (!error) return completion.failure;
  const auto cleanup = FilesystemError({"delete staging file", completion.stage.path(), error});
  if (!completion.failure.has_value()) return cleanup;
  return FlutterError("unavailable", completion.failure->message() +
      "; staging cleanup failed: " + cleanup.message(), completion.failure->details());
}

// Purpose: Stage complete bytes, then publish, with scope ownership covering every exit.
// @param write names the destination, bytes and publication policy.
// @returns No failure or a stop/filesystem error; recovery paths are disclosed when retained.
// @throws std::exception from effects/diagnostics after scope-owned staging cleanup.
std::optional<FlutterError> WriteBytes(const FileWrite& write) {
  if (IsStopRequested(write.mutation.stop_requested)) return StoppedMutation(0);
  auto destination = ContentDestination(write);
  if (const auto* error = std::get_if<FlutterError>(&destination)) return *error;
  const auto& path = std::get<fs::path>(destination);
  StagingFile stage;
  auto failure = CreateStage({.destination = path, .stage = stage,
                              .stop_requested = write.mutation.stop_requested});
  if (!failure.has_value()) failure = WriteStage({stage, write});
  if (!failure.has_value()) failure = FlushStage({stage, write});
  if (!failure.has_value()) failure = PublishStage({.stage = stage, .destination = path, .write = write});
  return FinishStage({stage, failure});
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

// Purpose: Validate and create one file or directory.
// @param mutation is the insert request; values.path names the target.
// @returns The affected count and insertedId, or an error.
// @throws std::exception from effects or diagnostics after scope-owned cleanup.
ErrorOr<EncodableMap> Insert(const FileMutation& mutation) {
  const EncodableMap* values = MapAt(mutation.request, "values");
  if (values == nullptr) return FlutterError("invalid-query", "simple_query: insert requires values");
  const auto path_text = StringAt(*values, "path");
  if (!path_text.has_value() || path_text->empty()) {
    return FlutterError("invalid-query", "simple_query: insert requires values.path");
  }
  auto contents = NewContents(*values);
  if (auto* invalid = std::get_if<FlutterError>(&contents)) return *invalid;
  if (IsStopRequested(mutation.stop_requested)) return StoppedMutation(0);
  const fs::path path = PathFromUtf8(*path_text);
  const auto& text = std::get<std::optional<std::string>>(contents);
  const std::string bytes = text.value_or(std::string());
  if (auto failure = InsertEntry({.path = path, .bytes = bytes, .exclusive = true,
                                   .mutation = mutation})) return *failure;
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
  if (IsStopRequested(mutation.stop_requested)) return StoppedMutation(0);
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
  auto targets = UpdateTargets(mutation);
  if (targets.has_error()) return targets.error();

  int64_t updated = 0;
  for (const auto& target : targets.value()) {
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
// @throws std::exception from injected callbacks after scope-owned cleanup;
// filesystem failures are returned as errors.
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
