#include "file_write.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>
#include <variant>

#include "file_mutation_errors.h"
#include "structured_log.h"

namespace simple_query_windows {
namespace file_mutation_detail {
namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
namespace fs = std::filesystem;

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

}  // namespace

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

}  // namespace file_mutation_detail
}  // namespace simple_query_windows
