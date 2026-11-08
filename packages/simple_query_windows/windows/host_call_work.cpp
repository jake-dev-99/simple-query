#include "host_call_work.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <utility>

#include "boundary_formats.h"
#include "file_mutations.h"
#include "file_records.h"
#include "request_values.h"
#include "winrt_records.h"

namespace simple_query_windows {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

// Purpose: Hold the rows a domain read returned, or the error Dart receives.
using RowsOrError = std::variant<std::vector<EncodableMap>, FlutterError>;

// Purpose: State what one domain supports on this machine.
struct DomainCapability {
  std::string domain;
  bool can_read = false;
  bool can_write = false;
  bool can_observe = false;
  bool can_stream = false;
  // Why a capability is missing, when Dart should show the user.
  std::optional<std::string> reason;
};

// Purpose: Build the capability entry Dart reads for one domain.
// @param capability states what the domain supports.
// @returns The capability map.
// @throws Nothing.
EncodableMap CapabilityMap(const DomainCapability& capability) {
  EncodableMap map;
  map[EncodableValue("domain")] = EncodableValue(capability.domain);
  map[EncodableValue("canRead")] = EncodableValue(capability.can_read);
  map[EncodableValue("canWrite")] = EncodableValue(capability.can_write);
  map[EncodableValue("canObserve")] = EncodableValue(capability.can_observe);
  map[EncodableValue("canStream")] = EncodableValue(capability.can_stream);
  if (capability.reason.has_value()) {
    map[EncodableValue("reason")] = EncodableValue(*capability.reason);
  }
  return map;
}

// Purpose: State a store-backed domain's capability from a probe of its store.
// @param domain is the domain name.
// @param probe is the outcome of listing the domain's stores.
// @returns Read and observe when the probe succeeded; otherwise the probe's
// error as the reason.
// @throws Nothing.
DomainCapability StoreCapability(const std::string& domain, const RowsResult& probe) {
  const bool readable = !probe.error.has_value();
  return {.domain = domain,
          .can_read = readable,
          .can_write = false,
          .can_observe = readable,
          .can_stream = false,
          .reason = probe.error};
}

// Purpose: Convert a read's outcome into the rows or the error Dart receives.
// @param result is the read's outcome.
// @returns The rows; permission-denied when access is off; unavailable for
// any other failure.
// @throws Nothing.
RowsOrError RowsOrFlutterError(RowsResult result) {
  if (!result.error.has_value()) {
    return std::move(result.rows);
  }
  if (result.denied_capability.has_value()) {
    return PermissionDeniedError(*result.error, *result.denied_capability);
  }
  return FlutterError("unavailable", *result.error);
}

// Purpose: Wrap a list of rows for a channel reply.
// @param rows are the rows.
// @returns The rows as one list value.
// @throws Nothing.
EncodableList RowsAsList(std::vector<EncodableMap> rows) {
  EncodableList list;
  list.reserve(rows.size());
  for (auto& row : rows) {
    list.push_back(EncodableValue(std::move(row)));
  }
  return list;
}

// Purpose: Name one domain read and when it gives up.
struct DomainRead {
  // files, media, contacts or calendar.
  const std::string& domain;
  // Resolves the root of a files or media read.
  const std::function<std::filesystem::path()>& root_for_files;
  // Ends the read early; the read then reports an error.
  const StopRequested& stop_requested;
};

// Purpose: Read every row of one readable domain.
// @param read names the domain, its root and the stop check.
// @returns The rows, or the error Dart receives.
// @throws Nothing; failures are returned as errors.
RowsOrError ReadDomainRows(const DomainRead& read) {
  if (read.domain == "contacts") {
    return RowsOrFlutterError(ListContactRecords(read.stop_requested));
  }
  if (read.domain == "calendar") {
    return RowsOrFlutterError(ListCalendarRecords(read.stop_requested));
  }
  try {
    return RowsOrFlutterError(ListRecords({.root = read.root_for_files(),
                                           .media_only = read.domain == "media",
                                           .stop_requested = read.stop_requested}));
  } catch (const std::system_error& error) {
    return FlutterError("unavailable",
                        std::string("simple_query: windows could not resolve the "
                                    "query root. Check platformData.rootPath - ") +
                            error.what());
  }
}

// Purpose: Give a resolved root to code that asks for it lazily.
// @param root is the root, or why it could not be resolved.
// @returns A function giving the root.
// @throws Nothing; the returned function throws std::system_error when the
// root could not be resolved.
std::function<std::filesystem::path()> RootResolver(ResolvedRoot root) {
  return [root = std::move(root)]() {
    if (root.error.has_value()) {
      throw *root.error;
    }
    return *root.path;
  };
}

}  // namespace

// Purpose: Report whether a domain lists files from a root directory.
// @param domain is the domain name.
// @returns true for files and media.
// @throws Nothing.
bool IsFileDomain(const std::string& domain) {
  return domain == "files" || domain == "media";
}

// Purpose: Report whether this host can query and observe a domain.
// @param domain is the domain name.
// @returns true for files, media, contacts and calendar.
// @throws Nothing.
bool IsReadableDomain(const std::string& domain) {
  return IsFileDomain(domain) || domain == "contacts" || domain == "calendar";
}

// Purpose: Build the error for a request that names no domain.
// @returns An invalid-query error naming the domains this host serves.
// @throws Nothing.
FlutterError MissingDomainError() {
  return FlutterError("invalid-query",
                      "simple_query: the request names no domain. Set domain to "
                      "files, media, contacts or calendar.");
}

// Purpose: Build the error for an operation this host does not offer.
// @param operation names the host call, such as query.
// @param domain is the domain the request named.
// @returns A not-supported error naming the domains that do support it.
// @throws Nothing.
FlutterError NotSupportedError(const std::string& operation, const std::string& domain) {
  const std::string supported =
      operation == "query" || operation == "observe" ? "files, media, contacts or calendar"
                                                     : "files or media";
  return FlutterError("not-supported", "simple_query: " + operation +
                                           " is not supported for domain " + domain +
                                           " on Windows. Use " + supported + ".");
}

// Purpose: Build the error for a read the user has not granted. The
// permission id rides in details, as on Android, so Dart can send the user
// to the right setting.
// @param message says what was denied and how to allow it.
// @param capability is the Windows capability, such as "contacts".
// @returns A permission-denied error carrying {permissions: [capability]}.
// @throws Nothing.
FlutterError PermissionDeniedError(const std::string& message, const std::string& capability) {
  EncodableMap details;
  details[EncodableValue("permissions")] = EncodableValue(EncodableList{EncodableValue(capability)});
  return FlutterError("permission-denied", message, EncodableValue(details));
}

// Purpose: Answer a store-listing extension with the stores read.
// @param key is the reply key, such as stores.
// @param listed is the outcome of listing the stores.
// @returns The stores under key, or the error Dart receives.
// @throws Nothing.
ErrorOr<std::optional<EncodableMap>> StoresReply(const std::string& key,
                                                 RowsResult listed) {
  RowsOrError rows = RowsOrFlutterError(std::move(listed));
  if (auto* error = std::get_if<FlutterError>(&rows)) {
    return *error;
  }
  EncodableMap response;
  response[EncodableValue(key)] =
      EncodableValue(RowsAsList(std::get<std::vector<EncodableMap>>(std::move(rows))));
  return std::make_optional(std::move(response));
}

// Purpose: List what each domain supports on this machine.
// @param stop_requested ends the store probes early.
// @returns The capabilities and the platform extensions.
// @throws Nothing; an unreadable store is reported in its capability.
ErrorOr<EncodableMap> ReadCapabilities(const StopRequested& stop_requested) {
  // Listing stores asks for access without reading every record.
  const std::vector<DomainCapability> capabilities{
      StoreCapability("contacts", ListContactStores(stop_requested)),
      {.domain = "media", .can_read = true, .can_write = true, .can_observe = true,
       .can_stream = true, .reason = std::nullopt},
      {.domain = "files", .can_read = true, .can_write = true, .can_observe = true,
       .can_stream = true, .reason = std::nullopt},
      StoreCapability("calendar", ListCalendarStores(stop_requested)),
      {.domain = "messages", .reason = "simple_query: messages is not supported on Windows"},
      {.domain = "calls", .reason = "simple_query: calls is not supported on Windows"},
      {.domain = "platformSpecific", .can_read = true, .reason = std::nullopt},
  };
  EncodableList capability_maps;
  for (const auto& capability : capabilities) {
    capability_maps.push_back(EncodableValue(CapabilityMap(capability)));
  }

  EncodableMap extensions;
  extensions[EncodableValue("windows.contacts")] = EncodableValue(true);
  extensions[EncodableValue("windows.calendar")] = EncodableValue(true);
  extensions[EncodableValue("windows.storage")] = EncodableValue(true);

  EncodableMap result;
  result[EncodableValue("capabilities")] = EncodableValue(capability_maps);
  result[EncodableValue("platformExtensions")] = EncodableValue(extensions);
  return result;
}

// Purpose: Read and shape one validated query.
// @param query is the domain, shape and root.
// @param stop_requested ends the read early.
// @returns One page of records with the total count, or the read's error.
// @throws Nothing.
ErrorOr<EncodableMap> RunQuery(const ValidatedQuery& query, const StopRequested& stop_requested) {
  const std::function<std::filesystem::path()> resolve_root = RootResolver(query.root);
  RowsOrError rows = ReadDomainRows(
      {.domain = query.domain, .root_for_files = resolve_root, .stop_requested = stop_requested});
  if (auto* error = std::get_if<FlutterError>(&rows)) {
    return *error;
  }
  return ShapeQueryResult(std::get<std::vector<EncodableMap>>(std::move(rows)), query.shape);
}

// Purpose: Run one files or media mutation request.
// @param request names the domain, the mutation type and its values.
// @param root is the request's root, resolved on the platform thread.
// @returns The affected count, or an error for an invalid request, another
// domain, or a filesystem step that failed.
// @throws Nothing.
ErrorOr<EncodableMap> MutateRequest(const EncodableMap& request, const ResolvedRoot& root) {
  const std::optional<std::string> domain = StringAt(request, "domain");
  if (!domain.has_value()) {
    return MissingDomainError();
  }
  if (!IsFileDomain(*domain)) {
    return NotSupportedError("mutate", *domain);
  }
  const std::function<std::filesystem::path()> resolve_root = RootResolver(root);
  return MutateFiles({.request = request, .domain = *domain, .resolve_root = resolve_root});
}

// Purpose: Run batch operations in order, stopping at the first failure.
// @param operations are the mutations, each with its root.
// @param stop_requested ends the batch between operations.
// @returns Each mutation's result, or the first error.
// @throws Nothing.
ErrorOr<EncodableMap> RunBatch(const std::vector<BatchOperation>& operations,
                               const StopRequested& stop_requested) {
  EncodableList results;
  for (const auto& operation : operations) {
    if (IsStopRequested(stop_requested)) {
      return FlutterError("unavailable", kReadStoppedError);
    }
    ErrorOr<EncodableMap> result = MutateRequest(operation.request, operation.root);
    if (result.has_error()) {
      return result.error();
    }
    results.push_back(EncodableValue(result.value()));
  }
  EncodableMap response;
  response[EncodableValue("results")] = EncodableValue(results);
  return response;
}

// Purpose: Describe a file so Dart can read its bytes, before it has a
// handle.
// @param path is the file's path.
// @returns The UTF-8 path, MIME type, size and metadata, or unavailable when
// the file is missing or its metadata cannot be read.
// @throws Nothing.
ErrorOr<EncodableMap> DescribeBinaryFile(const std::filesystem::path& path) {
  try {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
      return FlutterError("unavailable",
                          "simple_query: openBinary found no file at " + PathToUtf8(path) +
                              ". Query again for its current path.");
    }
    EncodableMap result;
    if (std::filesystem::is_regular_file(status)) {
      const auto size = std::filesystem::file_size(path, error);
      if (error) {
        return FlutterError("unavailable", "simple_query: openBinary could not read the "
                                           "size of " + PathToUtf8(path) + " - " +
                                               error.message());
      }
      result[EncodableValue("size")] = EncodableValue(static_cast<int64_t>(size));
    }
    result[EncodableValue("localPath")] = EncodableValue(PathToUtf8(path));
    result[EncodableValue("mimeType")] = EncodableValue(MimeFromPath(path));
    EncodableMap metadata;
    metadata[EncodableValue("source")] = EncodableValue("windows-host");
    result[EncodableValue("metadata")] = EncodableValue(metadata);
    return result;
  } catch (const std::system_error& error) {
    return FlutterError("unavailable",
                        std::string("simple_query: openBinary could not read the path. "
                                    "Check that it is valid Unicode - ") +
                            error.what());
  }
}

// Purpose: Take the snapshot an observer starts from, before its start is
// acknowledged, so a change right after the reply is never absorbed.
// @param spec is the observer's spec.
// @param stop_requested ends the snapshot early.
// @returns The snapshot; permission-denied, with the permission id, when
// access is off; unavailable when the records cannot be read.
// @throws Nothing.
BaselineOrError TakeBaseline(const ObserverSpec& spec, const StopRequested& stop_requested) {
  try {
    return spec.take_snapshot(stop_requested);
  } catch (const ReadDeniedError& error) {
    return PermissionDeniedError(error.what(), error.capability());
  } catch (const std::exception& error) {
    // Every snapshot failure lands here because nothing above the worker can
    // catch it; Dart gets it as the reply instead of a running observer.
    return FlutterError("unavailable",
                        std::string("simple_query: windows observe could not read "
                                    "the records to watch. Check the root or access, "
                                    "then start again - ") +
                            error.what());
  }
}

}  // namespace simple_query_windows
