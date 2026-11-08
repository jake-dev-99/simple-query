#ifndef SIMPLE_QUERY_WINDOWS_HOST_CALL_WORK_H_
#define SIMPLE_QUERY_WINDOWS_HOST_CALL_WORK_H_

// The parts of the host calls that read or write stores and the filesystem.
// They run on the background worker, so each takes everything it reads by
// value or by reference to the caller's copy, and none touches plugin state.

#include <flutter/encodable_value.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "native_query.g.h"
#include "polling_observers.h"
#include "query_shaping.h"
#include "rows_result.h"
#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Report whether a domain lists files from a root directory.
// @param domain is the domain name.
// @returns true for files and media.
// @throws Nothing.
bool IsFileDomain(const std::string& domain);

// Purpose: Report whether this host can query and observe a domain.
// @param domain is the domain name.
// @returns true for files, media, contacts and calendar.
// @throws Nothing.
bool IsReadableDomain(const std::string& domain);

// Purpose: Build the error for a request that names no domain.
// @returns An invalid-query error naming the domains this host serves.
// @throws Nothing.
FlutterError MissingDomainError();

// Purpose: Build the error for an operation this host does not offer.
// @param operation names the host call, such as query.
// @param domain is the domain the request named.
// @returns A not-supported error naming the domains that do support it.
// @throws Nothing.
FlutterError NotSupportedError(const std::string& operation, const std::string& domain);

// Purpose: Build the error for a read the user has not granted. The
// permission id rides in details, as on Android, so Dart can send the user
// to the right setting.
// @param message says what was denied and how to allow it.
// @param capability is the Windows capability, such as "contacts".
// @returns A permission-denied error carrying {permissions: [capability]}.
// @throws Nothing.
FlutterError PermissionDeniedError(const std::string& message, const std::string& capability);

// Purpose: Hold a request's root, resolved on the platform thread, or why it
// could not be resolved. The failure is kept rather than reported at once,
// because only a read or a filtered mutation needs the root.
struct ResolvedRoot {
  std::optional<std::filesystem::path> path;
  std::optional<std::system_error> error;
};

// Purpose: Answer a store-listing extension with the stores read.
// @param key is the reply key, such as stores.
// @param listed is the outcome of listing the stores.
// @returns The stores under key, or the error Dart receives.
// @throws Nothing.
ErrorOr<std::optional<flutter::EncodableMap>> StoresReply(const std::string& key,
                                                 RowsResult listed);

// Purpose: List what each domain supports on this machine.
// @param stop_requested ends the store probes early.
// @returns The capabilities and the platform extensions.
// @throws Nothing; an unreadable store is reported in its capability.
ErrorOr<flutter::EncodableMap> ReadCapabilities(const StopRequested& stop_requested);

// Purpose: Name one query that was validated on the platform thread.
struct ValidatedQuery {
  std::string domain;
  QueryShape shape;
  ResolvedRoot root;
};

// Purpose: Read and shape one validated query.
// @param query is the domain, shape and root.
// @param stop_requested ends the read early.
// @returns One page of records with the total count, or the read's error.
// @throws Nothing.
ErrorOr<flutter::EncodableMap> RunQuery(const ValidatedQuery& query, const StopRequested& stop_requested);

// Purpose: Run one files or media mutation request.
// @param request names the domain, the mutation type and its values.
// @param root is the request's root, resolved on the platform thread.
// @returns The affected count, or an error for an invalid request, another
// domain, or a filesystem step that failed.
// @throws Nothing.
ErrorOr<flutter::EncodableMap> MutateRequest(const flutter::EncodableMap& request, const ResolvedRoot& root);

// Purpose: Hold one batch operation and its root.
struct BatchOperation {
  flutter::EncodableMap request;
  ResolvedRoot root;
};

// Purpose: Run batch operations in order, stopping at the first failure.
// @param operations are the mutations, each with its root.
// @param stop_requested ends the batch between operations.
// @returns Each mutation's result, or the first error.
// @throws Nothing.
ErrorOr<flutter::EncodableMap> RunBatch(const std::vector<BatchOperation>& operations,
                               const StopRequested& stop_requested);

// Purpose: Describe a file so Dart can read its bytes, before it has a
// handle.
// @param path is the file's path.
// @returns The UTF-8 path, MIME type, size and metadata, or unavailable when
// the file is missing or its metadata cannot be read.
// @throws Nothing.
ErrorOr<flutter::EncodableMap> DescribeBinaryFile(const std::filesystem::path& path);

// Purpose: Hold an observer's first snapshot, or the error Dart receives.
using BaselineOrError = std::variant<Snapshot, FlutterError>;

// Purpose: Take the snapshot an observer starts from, before its start is
// acknowledged, so a change right after the reply is never absorbed.
// @param spec is the observer's spec.
// @param stop_requested ends the snapshot early.
// @returns The snapshot; permission-denied, with the permission id, when
// access is off; unavailable when the records cannot be read.
// @throws Nothing.
BaselineOrError TakeBaseline(const ObserverSpec& spec, const StopRequested& stop_requested);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_HOST_CALL_WORK_H_
