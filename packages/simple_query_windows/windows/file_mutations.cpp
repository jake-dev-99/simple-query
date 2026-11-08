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

// Purpose: Turn a requested stop into a failure with already confirmed effects.
// @param stop_requested reports cancellation; empty means never.
// @param affected is the confirmed effect count before stopping.
// @returns A stopped error, or no failure while the mutation remains active.
// @throws std::exception from the stop callback or error allocation.
std::optional<FlutterError> CheckMutationStop(const StopRequested& stop_requested,
                                             int64_t affected = 0) {
  if (IsStopRequested(stop_requested)) return StoppedMutation(affected);
  return std::nullopt;
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
    if (auto stopped = CheckMutationStop(creation.stop_requested)) return stopped;
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
  if (auto stopped = CheckMutationStop(write.mutation.stop_requested)) return stopped;
  const auto error = write.mutation.write_effects.write
      ? write.mutation.write_effects.write(operation.stage.handle(), write.bytes)
      : WriteAll({.handle = operation.stage.handle(), .bytes = write.bytes,
                  .stop_requested = write.mutation.stop_requested});
  if (error == std::errc::operation_canceled) return StoppedMutation(0);
  if (error) return FilesystemError({"write staging file", operation.stage.path(), error});
  return std::nullopt;
}

// Purpose: Flush one owned native stage through the injected or real boundary.
// @param operation supplies the owned handle and flush effect.
// @returns The native flush error, or no error.
// @throws std::exception from an injected flush callback; ownership remains with the stage.
std::error_code FlushNative(const StageOperation& operation) {
  const auto& effect = operation.write.mutation.write_effects.flush;
  return effect ? effect(operation.stage.handle())
      : (FlushFileBuffers(operation.stage.handle()) ? std::error_code{} : LastFileError());
}

// Purpose: Flush and close a complete stage before publishing its path.
// @param operation supplies the owner, stop check and flush effect.
// @returns No failure, a stopped error or a native flush/close error.
// @throws std::exception from effects or allocation; staging remains scope-owned.
std::optional<FlutterError> FlushStage(const StageOperation& operation) {
  const auto& mutation = operation.write.mutation;
  if (auto stopped = CheckMutationStop(mutation.stop_requested)) return stopped;
  const auto error = FlushNative(operation);
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

// Purpose: Hold prepared native replacement paths without changing public diagnostics.
// @param destination is the native original-file path.
// @param backup is the logical recovery path used in diagnostics.
// @param native_backup is the API-adapted backup path.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct ReplacementPaths {
  fs::path destination;
  fs::path backup;
  fs::path native_backup;
};

// Purpose: Prepare all replacement path allocations before recovery acquisition.
// @param publication names the logical destination.
// @param recovery names the prepared recovery owner.
// @returns The logical backup and API-adapted replacement paths.
// @throws std::exception during path conversion or allocation, before reserving recovery.
ReplacementPaths PrepareReplacement(const FilePublication& publication,
                                     const RecoveryDirectory& recovery) {
  auto backup = recovery.path() / "original";
  auto destination = NativeFilePath(publication.destination);
  auto native_backup = NativeFilePath(backup);
  return {std::move(destination), std::move(backup), std::move(native_backup)};
}

// Purpose: Construct a scope owner before reserving an independent recovery name.
// @param destination supplies the parent for the short recovery component.
// @returns A prepared, unacquired recovery owner using guaranteed copy elision.
// @throws std::exception during path/log preparation, before native acquisition.
RecoveryDirectory CreateRecovery(const fs::path& destination) {
  return RecoveryDirectory(ShortSibling(destination, ".recovery"));
}

// Purpose: Reserve recovery exclusively and stop before any original can enter it.
// @param recovery owns any successful reservation immediately.
// @param stop_requested reports cancellation before replacement starts.
// @returns No failure, a reservation failure or a stopped error.
// @throws std::exception from stop checks or diagnostics; the empty directory is scope-owned.
std::optional<FlutterError> PrepareRecovery(RecoveryDirectory& recovery,
                                          const StopRequested& stop_requested) {
  if (const auto error = recovery.Reserve()) {
    return FilesystemError({"reserve recovery directory", recovery.path(), error});
  }
  return CheckMutationStop(stop_requested);
}

// Purpose: Name a native replacement whose recovery is already retained by its caller.
// @param publication owns the stage and supplies the replacement effect.
// @param paths are the prepared native API paths.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct NativeReplacement {
  const FilePublication& publication;
  const ReplacementPaths& paths;
};

// Purpose: Perform only the injected or native replacement boundary.
// @param native names the stage, prepared paths and replacement effect.
// @returns The native replacement error, or no error.
// @throws std::exception from effects; caller-owned recovery already retains originals.
std::error_code RunReplacement(const NativeReplacement& native) {
  const FileReplacement replacement{.original = native.paths.destination,
      .replacement = native.publication.stage.native_path(), .backup = native.paths.native_backup};
  const auto& effect = native.publication.write.mutation.write_effects.replace;
  return effect ? effect(replacement)
      : (ReplaceFileW(replacement.original.c_str(), replacement.replacement.c_str(),
                      replacement.backup.c_str(), 0, nullptr, nullptr)
             ? std::error_code{} : LastFileError());
}

// Purpose: Name replacement outcome processing while both scope owners remain alive.
// @param publication owns the stage and names the logical destination.
// @param recovery retains any original copy until explicit successful cleanup.
// @param paths name the original backup.
// @param error is the native replacement outcome.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct ReplacementCompletion {
  const FilePublication& publication;
  RecoveryDirectory& recovery;
  const ReplacementPaths& paths;
  std::error_code error;
};

// Purpose: Disclose failed recovery or clean up an original after successful replacement.
// @param completion keeps both scope owners alive throughout outcome processing.
// @returns No failure or a disclosed recovery/cleanup failure.
// @throws std::exception during diagnostics; retained originals are never scope-deleted.
std::optional<FlutterError> FinishReplacement(const ReplacementCompletion& completion) {
  if (completion.error) {
    return RecoveryError({FilesystemError({"replace staged file", completion.publication.destination,
                                           completion.error}), completion.recovery.path(), 0});
  }
  completion.publication.stage.Published();
  return CleanupReplacement({completion.recovery, completion.paths.backup});
}

// Purpose: Orchestrate a replacement while retaining recovery throughout the native boundary.
// @param publication names the owned stage, logical destination and request.
// @returns No failure or a reservation/stop/recovery failure.
// @throws std::exception after scope-owned cleanup or intentional original retention.
std::optional<FlutterError> PublishReplacement(const FilePublication& publication) {
  auto recovery = CreateRecovery(publication.destination);
  const auto paths = PrepareReplacement(publication, recovery);
  if (auto failure = PrepareRecovery(recovery, publication.write.mutation.stop_requested)) {
    return failure;
  }
  recovery.Retain();
  const auto error = RunReplacement({publication, paths});
  return FinishReplacement({.publication = publication, .recovery = recovery,
                             .paths = paths, .error = error});
}

// Purpose: Publish a new file without replacing any destination that won a race.
// @param publication names the completed stage and exclusive destination.
// @returns No failure or a native publication error.
// @throws std::exception from native-path preparation or diagnostics; the stage remains owned.
std::optional<FlutterError> PublishInsert(const FilePublication& publication) {
  const auto destination = NativeFilePath(publication.destination);
  if (!MoveFileExW(publication.stage.native_path().c_str(), destination.c_str(),
                   MOVEFILE_WRITE_THROUGH)) {
    return FilesystemError({"publish staged file", publication.destination, LastFileError()});
  }
  publication.stage.Published();
  return std::nullopt;
}

// Purpose: Dispatch active publication according to its exclusive or replacement policy.
// @param publication owns the stage and supplies its publication policy.
// @returns No failure or a stop/native/recovery error.
// @throws std::exception from effects/diagnostics; resources remain scope-owned.
std::optional<FlutterError> PublishStage(const FilePublication& publication) {
  if (auto stopped = CheckMutationStop(publication.write.mutation.stop_requested)) return stopped;
  return publication.write.exclusive ? PublishInsert(publication) : PublishReplacement(publication);
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

// Purpose: Name one staged write's preparation, content and publication sequence.
// @param stage owns acquired staging resources through every operation.
// @param destination is the already resolved logical destination.
// @param write supplies validated contents, effects and cancellation.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct StagedWrite {
  StagingFile& stage;
  const fs::path& destination;
  const FileWrite& write;
};

// Purpose: Run focused creation, write, flush and publication steps in order.
// @param staged keeps the scope owner alive while operations may throw.
// @returns The first failure, or no failure after publication.
// @throws std::exception from effects or diagnostics; caller retains staging ownership.
std::optional<FlutterError> RunStagedWrite(const StagedWrite& staged) {
  auto failure = CreateStage({.destination = staged.destination, .stage = staged.stage,
                              .stop_requested = staged.write.mutation.stop_requested});
  if (!failure.has_value()) failure = WriteStage({staged.stage, staged.write});
  if (!failure.has_value()) failure = FlushStage({staged.stage, staged.write});
  if (!failure.has_value()) {
    failure = PublishStage({.stage = staged.stage, .destination = staged.destination,
                            .write = staged.write});
  }
  return failure;
}

// Purpose: Resolve and scope-own one staged write through publication and explicit cleanup.
// @param write names the destination, bytes and publication policy.
// @returns No failure or a stop/filesystem error, disclosing retained recovery.
// @throws std::exception after scope-owned staging cleanup or intentional recovery retention.
std::optional<FlutterError> WriteBytes(const FileWrite& write) {
  if (auto stopped = CheckMutationStop(write.mutation.stop_requested)) return stopped;
  auto destination = ContentDestination(write);
  if (const auto* error = std::get_if<FlutterError>(&destination)) return *error;
  StagingFile stage;
  auto failure = RunStagedWrite({.stage = stage, .destination = std::get<fs::path>(destination),
                                 .write = write});
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
