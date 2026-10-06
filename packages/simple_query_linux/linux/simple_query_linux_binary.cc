#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "simple_query_linux_plugin_private.h"

namespace simple_query_linux {

namespace {

/** Purpose: Close one Linux descriptor on every OpenBinary exit path.
 * @param value owns one descriptor, or -1 when this owner is empty.
 * @returns Unique scoped ownership of the descriptor.
 * @throws Nothing. */
struct FileDescriptor {
  int value = -1;

  /** Purpose: Create an empty descriptor owner.
   * @param None.
   * @returns An owner that closes nothing.
   * @throws Nothing. */
  FileDescriptor() = default;

  /** Purpose: Take unique ownership of one open descriptor.
   * @param descriptor is closed when this owner leaves scope.
   * @returns An owner for descriptor.
   * @throws Nothing. */
  explicit FileDescriptor(int descriptor) : value(descriptor) {}

  /** Purpose: Prevent duplicate ownership of one descriptor.
   * @param other is intentionally not copied.
   * @returns Nothing because construction is disabled.
   * @throws Nothing. */
  FileDescriptor(const FileDescriptor& other) = delete;

  /** Purpose: Prevent duplicate assignment of one descriptor.
   * @param other is intentionally not assigned.
   * @returns Nothing because assignment is disabled.
   * @throws Nothing. */
  FileDescriptor& operator=(const FileDescriptor& other) = delete;

  /** Purpose: Close the owned descriptor.
   * @param None.
   * @returns Nothing.
   * @throws Nothing. */
  ~FileDescriptor() {
    if (value >= 0) close(value);
  }
};

}  // namespace

/** Purpose: Validate and expose one filesystem resource as a binary handle.
 * @param request identifies the native resource.
 * @returns Handle metadata or a structured error.
 * @throws Native allocation exceptions for the outer callback boundary to
 * translate. */
ValueResult NativeQueryHostApiImpl::OpenBinary(FlValue* request) {
  const std::string domain = StringOr(request, "domain", "platformSpecific");
  if (domain != "files" && domain != "media") {
    return Failure("not-supported",
                   "simple_query: openBinary is not supported for domain " +
                       domain + " on Linux host");
  }

  std::optional<std::string> path;
  FlValue* platform_data = AsMap(FindValue(request, "platformData"));
  if (platform_data != nullptr) {
    path = AsString(FindValue(platform_data, "path"));
  }
  if (!path.has_value()) {
    path = AsString(FindValue(request, "recordId"));
  }
  if (!path.has_value() || path->empty()) {
    return Failure(
        "invalid-query",
        "simple_query: openBinary requires recordId or platformData.path");
  }

  const std::filesystem::path binary_path(*path);
  FileDescriptor descriptor(
      open(binary_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  if (descriptor.value < 0) {
    return Failure(
        "unavailable",
        FileSystemFailure("binary read-only open", binary_path,
                          std::error_code(errno, std::generic_category())));
  }
  struct stat stat_info {};
  if (fstat(descriptor.value, &stat_info) != 0) {
    return Failure(
        "unavailable",
        FileSystemFailure("binary metadata check", binary_path,
                          std::error_code(errno, std::generic_category())));
  }
  if (!S_ISREG(stat_info.st_mode)) {
    return Failure("unavailable",
                   "simple_query: binary resource is not a regular file: " +
                       binary_path.string());
  }

  const int64_t next_handle = handle_counter_ + 1;
  const auto handle =
      std::string("linux_handle_") + std::to_string(next_handle);
  const std::string mime_type = MimeFromPath(binary_path);
  auto result = Value(fl_value_new_map());
  MapSetString(result.get(), "handleId", handle);
  MapSetString(result.get(), "localPath", *path);
  MapSetString(result.get(), "mimeType", mime_type);
  MapSetInt(result.get(), "size", static_cast<int64_t>(stat_info.st_size));
  auto metadata = Value(fl_value_new_map());
  MapSetString(metadata.get(), "source", "linux-host");
  MapSet(result.get(), "metadata", metadata.release());
  open_handles_.emplace(handle, *path);
  handle_counter_ = next_handle;
  return Success(std::move(result));
}

/** Purpose: Release one binary handle idempotently.
 * @param handle_id selects the handle.
 * @returns No error after cleanup.
 * @throws Nothing. */
std::optional<NativeError> NativeQueryHostApiImpl::CloseBinary(
    const std::string& handle_id) {
  open_handles_.erase(handle_id);
  return std::nullopt;
}

}  // namespace simple_query_linux
