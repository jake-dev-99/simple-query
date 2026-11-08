#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <flutter/plugin_registrar_windows.h>

#include "boundary_formats.h"
#include "domain_snapshots.h"
#include "file_mutations.h"
#include "file_records.h"
#include "native_query.g.h"
#include "polling_observers.h"
#include "query_shaping.h"
#include "request_values.h"
#include "rows_result.h"
#include "structured_log.h"
#include "winrt_records.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// An observer that names no interval polls once a second; a shorter interval
// than the floor would keep a large tree's walk running back to back.
constexpr int64_t kDefaultPollingIntervalMs = 1000;
constexpr int64_t kMinimumPollingIntervalMs = 250;

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

// Purpose: Read the wall clock for event timestamps.
// @returns Milliseconds since the Unix epoch.
// @throws Nothing.
int64_t EpochMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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
// A denial carries the permission id in details, as on Android, so Dart can
// send the user to the right setting.
// @param result is the read's outcome.
// @returns The rows; permission-denied when access is off; unavailable for
// any other failure.
// @throws Nothing.
RowsOrError RowsOrFlutterError(RowsResult result) {
  if (!result.error.has_value()) {
    return std::move(result.rows);
  }
  if (result.denied_capability.has_value()) {
    EncodableMap details;
    details[EncodableValue("permissions")] =
        EncodableValue(EncodableList{EncodableValue(*result.denied_capability)});
    return FlutterError("permission-denied", *result.error, EncodableValue(details));
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

// Purpose: Find the directory a files or media request reads. Dart sends
// UTF-8, so the path is converted explicitly rather than through the system
// code page.
// @param request is the request; platformData.rootPath names the directory.
// @param current_directory reads the working directory, used when the
// request names none.
// @returns The named directory, or the working directory.
// @throws std::system_error when the named path is not valid UTF-8 or the
// working directory cannot be read.
std::filesystem::path ResolveRootPath(
    const EncodableMap& request,
    const std::function<std::filesystem::path()>& current_directory) {
  const EncodableMap* platform_data = MapAt(request, "platformData");
  if (platform_data != nullptr) {
    const auto root = StringAt(*platform_data, "rootPath");
    if (root.has_value() && !root->empty()) {
      return PathFromUtf8(*root);
    }
  }
  return current_directory();
}

// Purpose: Read every row of one readable domain.
// @param domain is files, media, contacts or calendar.
// @param root_for_files resolves the root of a files or media read.
// @returns The rows, or the error Dart receives.
// @throws Nothing; failures are returned as errors.
RowsOrError ReadDomainRows(const std::string& domain,
                           const std::function<std::filesystem::path()>& root_for_files) {
  if (domain == "contacts") {
    return RowsOrFlutterError(ListContactRecords({}));
  }
  if (domain == "calendar") {
    return RowsOrFlutterError(ListCalendarRecords({}));
  }
  try {
    return RowsOrFlutterError(
        ListRecords({.root = root_for_files(), .media_only = domain == "media"}));
  } catch (const std::system_error& error) {
    return FlutterError("unavailable",
                        std::string("simple_query: windows could not resolve the "
                                    "query root. Check platformData.rootPath - ") +
                            error.what());
  }
}

// Purpose: Reject arguments for an extension method that takes none.
// @param args are the method's arguments, or null.
// @param method names the method for the error.
// @returns An invalid-query error when arguments were sent, otherwise nothing.
// @throws Nothing.
std::optional<FlutterError> RejectArguments(const EncodableMap* args,
                                            const std::string& method) {
  if (args == nullptr || args->empty()) {
    return std::nullopt;
  }
  return FlutterError("invalid-query",
                      "simple_query: " + method + " takes no arguments. Call it without args.");
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

// Purpose: Build one known-folder entry for resolveKnownFolders.
// @param id names the folder.
// @param path is the folder's path.
// @returns The entry, with the path in UTF-8.
// @throws std::system_error when the path is not valid Unicode.
EncodableValue KnownFolder(const std::string& id, const std::filesystem::path& path) {
  EncodableMap folder;
  folder[EncodableValue("id")] = EncodableValue(id);
  folder[EncodableValue("path")] = EncodableValue(PathToUtf8(path));
  return EncodableValue(folder);
}

// Purpose: Answer windows.storage.resolveKnownFolders.
// @param args may hold includeTemp as a bool; absent means true.
// @param current_directory reads the working directory.
// @returns The current folder, and the temp folder unless excluded, or an
// error for a non-bool includeTemp or an unreadable folder.
// @throws Nothing.
ErrorOr<std::optional<EncodableMap>> ResolveKnownFolders(
    const EncodableMap* args, const std::function<std::filesystem::path()>& current_directory) {
  const EncodableValue* include_temp_value =
      args == nullptr ? nullptr : FindValue(*args, "includeTemp");
  const std::optional<bool> include_temp =
      include_temp_value == nullptr ? std::optional<bool>(true) : BoolAt(*args, "includeTemp");
  if (!include_temp.has_value()) {
    return FlutterError("invalid-query",
                        "simple_query: windows.storage.resolveKnownFolders expects "
                        "includeTemp as a bool. Send true or false.");
  }
  try {
    EncodableList folders{KnownFolder("current", current_directory())};
    if (*include_temp) {
      folders.push_back(KnownFolder("temp", std::filesystem::temp_directory_path()));
    }
    EncodableMap response;
    response[EncodableValue("folders")] = EncodableValue(folders);
    return std::make_optional(std::move(response));
  } catch (const std::system_error& error) {
    return FlutterError("unavailable",
                        std::string("simple_query: windows could not read a known "
                                    "folder - ") +
                            error.what());
  }
}

// Purpose: Answer windows.storage.listLibraries.
// @returns The library names this host recognizes.
// @throws Nothing.
ErrorOr<std::optional<EncodableMap>> ListLibraries() {
  EncodableList libraries{EncodableValue("documents"), EncodableValue("music"),
                          EncodableValue("pictures"), EncodableValue("videos")};
  EncodableMap response;
  response[EncodableValue("libraries")] = EncodableValue(libraries);
  return std::make_optional(std::move(response));
}

// Purpose: Name the file an openBinary request reads.
// @param request names the file by platformData.path or recordId.
// @returns The path text, or nothing when the request names none.
// @throws Nothing.
std::optional<std::string> BinaryPathText(const EncodableMap& request) {
  const EncodableMap* platform_data = MapAt(request, "platformData");
  std::optional<std::string> path =
      platform_data == nullptr ? std::nullopt : StringAt(*platform_data, "path");
  if (!path.has_value()) {
    path = StringAt(request, "recordId");
  }
  if (path.has_value() && path->empty()) {
    return std::nullopt;
  }
  return path;
}

// Purpose: Read the polling interval an observe request asks for.
// @param request may hold pollingIntervalMs as a number.
// @returns The interval, raised to the floor; the default when absent; or an
// error when it is not a number.
// @throws Nothing.
std::variant<std::chrono::milliseconds, FlutterError> PollingInterval(
    const EncodableMap& request) {
  const EncodableValue* value = FindValue(request, "pollingIntervalMs");
  if (value == nullptr) {
    return std::chrono::milliseconds(kDefaultPollingIntervalMs);
  }
  const std::optional<int64_t> interval_ms = AsInt(value);
  if (!interval_ms.has_value()) {
    return FlutterError("invalid-query",
                        "simple_query: pollingIntervalMs must be a number of "
                        "milliseconds. Send an integer.");
  }
  return std::chrono::milliseconds(std::max(kMinimumPollingIntervalMs, *interval_ms));
}

}  // namespace

// Purpose: Answer one engine's simple_query host calls on Windows.
// Create and destroy it on that engine's platform thread.
class NativeQueryHostApiImpl : public NativeQueryHostApi {
 public:
  // Purpose: Serve host calls and send observe events through one engine.
  // @param messenger is the engine's messenger; it must outlive this object.
  // @param effects supplies the environment reads.
  // @returns Nothing.
  // @throws Nothing.
  NativeQueryHostApiImpl(flutter::BinaryMessenger* messenger,
                         PluginEffects effects)
      : current_directory_(std::move(effects.current_directory)),
        flutter_api_(messenger),
        observers_({.send_event =
                        [this](const std::string& observer_id,
                               const EncodableMap& event) {
                          SendObserveEvent(observer_id, event);
                        },
                    .now_epoch_ms = &EpochMsNow}) {}

  // Purpose: Report what each domain supports on this machine.
  // @returns The capabilities and the platform extensions.
  // @throws Nothing; an unreadable store is reported in its capability.
  ErrorOr<EncodableMap> GetCapabilities() override {
    // Listing stores asks for access without reading every record.
    const std::vector<DomainCapability> capabilities{
        StoreCapability("contacts", ListContactStores({})),
        {.domain = "media", .can_read = true, .can_write = true, .can_observe = true,
         .can_stream = true, .reason = std::nullopt},
        {.domain = "files", .can_read = true, .can_write = true, .can_observe = true,
         .can_stream = true, .reason = std::nullopt},
        StoreCapability("calendar", ListCalendarStores({})),
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

  // Purpose: Answer one query for files, media, contacts or calendar.
  // @param request names the domain, filters, sort, page and projection.
  // @returns One page of records with the total count; invalid-query for a
  // malformed request; permission-denied, with the permission id, when access
  // is off; unavailable when the store cannot be read.
  // @throws Nothing.
  ErrorOr<EncodableMap> Query(const EncodableMap& request) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return MissingDomainError();
    }
    if (!IsReadableDomain(*domain)) {
      return NotSupportedError("query", *domain);
    }
    auto shape = ParseQueryShape(request);
    if (auto* invalid = std::get_if<std::string>(&shape)) {
      return FlutterError("invalid-query", *invalid);
    }
    RowsOrError rows = ReadDomainRows(
        *domain, [&]() { return ResolveRootPath(request, current_directory_); });
    if (auto* error = std::get_if<FlutterError>(&rows)) {
      return *error;
    }
    return ShapeQueryResult(std::get<std::vector<EncodableMap>>(std::move(rows)),
                            std::get<QueryShape>(shape));
  }

  // Purpose: Insert, update or delete files or media.
  // @param request names the domain, the mutation type and its values.
  // @returns The affected count, or an error for an invalid request or a
  // filesystem step that failed.
  // @throws Nothing.
  ErrorOr<EncodableMap> Mutate(const EncodableMap& request) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return MissingDomainError();
    }
    if (!IsFileDomain(*domain)) {
      return NotSupportedError("mutate", *domain);
    }
    const std::function<std::filesystem::path()> resolve_root = [&]() {
      return ResolveRootPath(request, current_directory_);
    };
    return MutateFiles({.request = request, .domain = *domain, .resolve_root = resolve_root});
  }

  // Purpose: Run several mutations in order, stopping at the first failure.
  // @param request holds the operations and a shared platformData.
  // @returns Each mutation's result, or the first error.
  // @throws Nothing.
  ErrorOr<EncodableMap> Batch(const EncodableMap& request) override {
    const EncodableList* operations = ListAt(request, "operations");
    if (operations == nullptr) {
      return FlutterError("invalid-query",
                          "simple_query: batch requires operations. Send a list of "
                          "mutation maps.");
    }
    const EncodableValue* shared_platform_data = FindValue(request, "platformData");

    EncodableList results;
    for (const auto& operation_value : *operations) {
      const auto* operation = std::get_if<EncodableMap>(&operation_value);
      if (operation == nullptr) {
        return FlutterError("invalid-query",
                            "simple_query: each batch operation must be a map.");
      }
      EncodableMap merged = *operation;
      if (FindValue(*operation, "platformData") == nullptr &&
          shared_platform_data != nullptr) {
        merged[EncodableValue("platformData")] = *shared_platform_data;
      }
      ErrorOr<EncodableMap> result = Mutate(merged);
      if (result.has_error()) {
        return result.error();
      }
      results.push_back(EncodableValue(result.value()));
    }

    EncodableMap response;
    response[EncodableValue("results")] = EncodableValue(results);
    return response;
  }

  // Purpose: Start polling one domain and sending its changes to Dart.
  // @param request names the domain, the polling interval and the root.
  // @returns The new observer's id; invalid-query for a malformed request; or
  // unavailable when the root cannot be resolved or the worker cannot start.
  // @throws Nothing.
  ErrorOr<std::string> ObserveStart(const EncodableMap& request) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return MissingDomainError();
    }
    if (!IsReadableDomain(*domain)) {
      return NotSupportedError("observe", *domain);
    }
    if (!observers_.IsReady()) {
      return FlutterError("unavailable",
                          "simple_query: windows observe could not create its "
                          "platform-thread message window. Restart the app.");
    }
    auto interval = PollingInterval(request);
    if (auto* invalid = std::get_if<FlutterError>(&interval)) {
      return *invalid;
    }

    // Only files and media read a root. It is resolved once, here, so the
    // worker keeps only what its snapshots read.
    std::filesystem::path root;
    if (IsFileDomain(*domain)) {
      try {
        root = ResolveRootPath(request, current_directory_);
      } catch (const std::system_error& error) {
        return FlutterError("unavailable",
                            std::string("simple_query: windows observe could not "
                                        "resolve its root directory. Check "
                                        "platformData.rootPath - ") +
                                error.what());
      }
    }
    ObserverSpec spec{
        .domain = *domain,
        .polling_interval = std::get<std::chrono::milliseconds>(interval),
        .take_snapshot =
            [root, domain = *domain](const StopRequested& stop_requested) {
              return BuildSnapshotForDomain(
                  {.root = root, .domain = domain, .stop_requested = stop_requested});
            },
    };
    try {
      return observers_.Start(std::move(spec));
    } catch (const std::system_error& error) {
      return FlutterError("unavailable",
                          std::string("simple_query: windows observe could not "
                                      "start its polling thread. Try again - ") +
                              error.what());
    }
  }

  // Purpose: Stop one observer. An unknown id does nothing.
  // @param observer_id is the id ObserveStart returned.
  // @returns Nothing on success.
  // @throws Nothing.
  std::optional<FlutterError> ObserveStop(const std::string& observer_id) override {
    observers_.Stop(observer_id);
    return std::nullopt;
  }

  // Purpose: Describe a file so Dart can read its bytes.
  // @param request names the file by platformData.path or recordId, in UTF-8.
  // @returns A handle, the file's UTF-8 path, MIME type and size, or an error.
  // @throws Nothing.
  ErrorOr<EncodableMap> OpenBinary(const EncodableMap& request) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return MissingDomainError();
    }
    if (!IsFileDomain(*domain)) {
      return NotSupportedError("openBinary", *domain);
    }
    const std::optional<std::string> path_text = BinaryPathText(request);
    if (!path_text.has_value()) {
      return FlutterError("invalid-query",
                          "simple_query: openBinary requires recordId or "
                          "platformData.path. Send the file's path.");
    }
    try {
      return DescribeBinary(PathFromUtf8(*path_text));
    } catch (const std::system_error& error) {
      return FlutterError("unavailable",
                          std::string("simple_query: openBinary could not read the "
                                      "path. Check that it is valid Unicode - ") +
                              error.what());
    }
  }

  // Purpose: Forget a handle OpenBinary returned.
  // @param handle_id is the handle to forget.
  // @returns Nothing on success.
  // @throws Nothing.
  std::optional<FlutterError> CloseBinary(const std::string& handle_id) override {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  // Purpose: Run one Windows-specific extension method.
  // @param extension_namespace is the extension namespace, such as
  // windows.storage.
  // @param method is the method name.
  // @param args are the method's arguments, or null.
  // @returns The method's result, or an error for an unknown method, invalid
  // arguments, a denied store or an unreadable store.
  // @throws Nothing.
  ErrorOr<std::optional<EncodableMap>> CallExtension(
      const std::string& extension_namespace, const std::string& method,
      const EncodableMap* args) override {
    const std::string qualified = extension_namespace + "." + method;
    if (qualified == "windows.contacts.listStores") {
      if (auto rejected = RejectArguments(args, qualified)) {
        return *rejected;
      }
      return StoresReply("stores", ListContactStores({}));
    }
    if (qualified == "windows.calendar.listCalendars") {
      if (auto rejected = RejectArguments(args, qualified)) {
        return *rejected;
      }
      return StoresReply("calendars", ListCalendarStores({}));
    }
    if (qualified == "windows.storage.resolveKnownFolders") {
      return ResolveKnownFolders(args, current_directory_);
    }
    if (qualified == "windows.storage.listLibraries") {
      if (auto rejected = RejectArguments(args, qualified)) {
        return *rejected;
      }
      return ListLibraries();
    }
    return FlutterError("not-supported", "simple_query: " + qualified +
                                             " is not supported on Windows. Check "
                                             "platformExtensions from getCapabilities.");
  }

 private:
  // Purpose: Register a handle for an existing file and describe it.
  // @param path is the file's path.
  // @returns The handle, UTF-8 path, MIME type and size, or unavailable when
  // the file is missing or its metadata cannot be read.
  // @throws std::system_error when the path is not valid Unicode.
  ErrorOr<EncodableMap> DescribeBinary(const std::filesystem::path& path) {
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
    const std::string handle = "win_handle_" + std::to_string(++handle_counter_);
    open_handles_[handle] = path;
    result[EncodableValue("handleId")] = EncodableValue(handle);
    result[EncodableValue("localPath")] = EncodableValue(PathToUtf8(path));
    result[EncodableValue("mimeType")] = EncodableValue(MimeFromPath(path));
    EncodableMap metadata;
    metadata[EncodableValue("source")] = EncodableValue("windows-host");
    result[EncodableValue("metadata")] = EncodableValue(metadata);
    return result;
  }

  // Purpose: Send one observe event to Dart. Runs on the platform thread.
  // A run of rejected events is logged once, because a Dart side that stopped
  // listening rejects every event after it. An accepted event ends the run,
  // so the next rejection is logged again.
  // @param observer_id is the observer the event belongs to.
  // @param event is the change event map.
  // @returns Nothing.
  // @throws Nothing.
  void SendObserveEvent(const std::string& observer_id, const EncodableMap& event) {
    flutter_api_.OnObserveEvent(
        observer_id, event,
        [reported = rejected_event_reported_]() { *reported = false; },
        [observer_id, reported = rejected_event_reported_](const FlutterError& error) {
          if (*reported) {
            return;
          }
          *reported = true;
          WriteLog({LogLevel::kWarning,
                    "observe_event_rejected",
                    {{"observer_id", observer_id},
                     {"code", error.code()},
                     {"message", error.message()},
                     {"next_log", "after an event is accepted"}}});
        });
  }

  // Read only on the platform thread.
  const std::function<std::filesystem::path()> current_directory_;
  NativeQueryFlutterApi flutter_api_;
  // Shared with pending replies, which can arrive after this object is gone.
  // Read and written only on the platform thread.
  std::shared_ptr<bool> rejected_event_reported_ = std::make_shared<bool>(false);
  int64_t handle_counter_ = 0;
  std::map<std::string, std::filesystem::path> open_handles_;
  // Declared after flutter_api_ so it is destroyed first: its destructor
  // stops every observer before the API it sends through goes away.
  PollingObservers observers_;
};

// Each engine's registrar owns its own plugin and host API, so every engine's
// events go to its own messenger.
// Purpose: Create this engine's plugin and hand it to the registrar.
// @param registrar is the engine's plugin registrar.
// @returns Nothing.
// @throws Nothing.
void SimpleQueryWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  registrar->AddPlugin(
      std::make_unique<SimpleQueryWindowsPlugin>(registrar->messenger()));
}

// Purpose: Start answering host API calls on one engine's messenger.
// @param messenger is the engine's messenger; it must outlive the plugin.
// @param effects supplies the environment reads.
// @returns Nothing.
// @throws Nothing.
SimpleQueryWindowsPlugin::SimpleQueryWindowsPlugin(
    flutter::BinaryMessenger* messenger, PluginEffects effects)
    : messenger_(messenger),
      host_api_(std::make_unique<NativeQueryHostApiImpl>(messenger,
                                                         std::move(effects))) {
  NativeQueryHostApi::SetUp(messenger_, host_api_.get());
}

// The registrar destroys its plugins while the messenger is still alive, so
// the handlers are removed and every observer stops before the engine goes.
// Purpose: Remove this engine's handlers, then stop every observer.
// @returns Nothing.
// @throws Nothing.
SimpleQueryWindowsPlugin::~SimpleQueryWindowsPlugin() {
  NativeQueryHostApi::SetUp(messenger_, nullptr);
}

}  // namespace simple_query_windows
