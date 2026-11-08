#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <flutter/plugin_registrar_windows.h>

#include "background_calls.h"
#include "host_call_work.h"
#include "boundary_formats.h"
#include "domain_snapshots.h"
#include "file_mutations.h"
#include "file_records.h"
#include "native_query.g.h"
#include "observe_event_sender.h"
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

// Purpose: Read the wall clock for event timestamps.
// @returns Milliseconds since the Unix epoch.
// @throws Nothing.
int64_t EpochMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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

// The most host calls that may be queued or awaiting their reply at once,
// so a stalled store or platform thread cannot grow memory without limit.
constexpr size_t kMaxUnfinishedCalls = 64;

// Purpose: Build the reply for a call the background worker did not take.
// @param result says why the call was not queued.
// @returns unavailable, saying whether to retry or restart.
// @throws std::logic_error for kQueued, which is not a refusal.
FlutterError NotQueuedError(EnqueueResult result) {
  switch (result) {
    case EnqueueResult::kFull:
      return FlutterError("unavailable",
                          "simple_query: windows has too many calls in progress. Try "
                          "again after earlier calls finish.");
    case EnqueueResult::kNotReady:
      return FlutterError("unavailable",
                          "simple_query: windows could not start its background worker. "
                          "Restart the app.");
    case EnqueueResult::kQueued:
      break;
  }
  throw std::logic_error("simple_query_windows: a queued call has no refusal error");
}

// Purpose: Describe one host call split between the background worker and
// the platform thread.
template <typename Value, typename Reply>
struct BackgroundCall {
  // Runs on the background worker. It must not throw, and must hold by copy
  // everything it reads, because the plugin can be destroyed while it runs.
  std::function<Value(const StopRequested&)> work;
  // Runs on the platform thread with work's value and builds the reply.
  std::function<ErrorOr<Reply>(Value)> finish;
  // Pigeon's reply callback. Runs on the platform thread.
  std::function<void(ErrorOr<Reply>)> result;
};

}  // namespace

// Purpose: Answer one engine's simple_query host calls on Windows.
// Calls that read or write stores or the filesystem run on a background
// worker and reply on the platform thread, so the platform thread never
// waits for I/O. Create and destroy it on that engine's platform thread.
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
        event_sender_(messenger),
        observers_({.send_event =
                        [this](const std::string& observer_id, const EncodableMap& event,
                               std::function<void()> on_settled) {
                          event_sender_.Send(observer_id, event, std::move(on_settled));
                        },
                    .now_epoch_ms = &EpochMsNow}),
        background_calls_(kMaxUnfinishedCalls) {}

  // Purpose: Report what each domain supports on this machine.
  // @param result receives the capabilities and the platform extensions.
  // @returns Nothing.
  // @throws Nothing; an unreadable store is reported in its capability.
  void GetCapabilities(std::function<void(ErrorOr<EncodableMap> reply)> result) override {
    RunInBackground<EncodableMap>(&ReadCapabilities, std::move(result));
  }

  // Purpose: Answer one query for files, media, contacts or calendar.
  // @param request names the domain, filters, sort, page and projection.
  // @param result receives one page of records with the total count;
  // invalid-query for a malformed request; permission-denied, with the
  // permission id, when access is off; unavailable when the store cannot be
  // read.
  // @returns Nothing.
  // @throws Nothing.
  void Query(const EncodableMap& request,
             std::function<void(ErrorOr<EncodableMap> reply)> result) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return result(MissingDomainError());
    }
    if (!IsReadableDomain(*domain)) {
      return result(NotSupportedError("query", *domain));
    }
    auto shape = ParseQueryShape(request);
    if (auto* invalid = std::get_if<std::string>(&shape)) {
      return result(FlutterError("invalid-query", *invalid));
    }
    ValidatedQuery query{.domain = *domain,
                         .shape = std::get<QueryShape>(std::move(shape)),
                         .root = ResolveRoot(request)};
    RunInBackground<EncodableMap>(
        [query = std::move(query)](const StopRequested& stop_requested) {
          return RunQuery(query, stop_requested);
        },
        std::move(result));
  }

  // Purpose: Insert, update or delete files or media.
  // @param request names the domain, the mutation type and its values.
  // @param result receives the affected count, or an error for an invalid
  // request or a filesystem step that failed.
  // @returns Nothing.
  // @throws Nothing.
  void Mutate(const EncodableMap& request,
              std::function<void(ErrorOr<EncodableMap> reply)> result) override {
    RunInBackground<EncodableMap>(
        [request, root = ResolveRoot(request)](const StopRequested&) {
          return MutateRequest(request, root);
        },
        std::move(result));
  }

  // Purpose: Run several mutations in order, stopping at the first failure.
  // @param request holds the operations and a shared platformData.
  // @param result receives each mutation's result, or the first error.
  // @returns Nothing.
  // @throws Nothing.
  void Batch(const EncodableMap& request,
             std::function<void(ErrorOr<EncodableMap> reply)> result) override {
    const EncodableList* operations = ListAt(request, "operations");
    if (operations == nullptr) {
      return result(FlutterError("invalid-query",
                                 "simple_query: batch requires operations. Send a list "
                                 "of mutation maps."));
    }
    const EncodableValue* shared_platform_data = FindValue(request, "platformData");
    std::vector<BatchOperation> batch;
    for (const auto& operation_value : *operations) {
      const auto* operation = std::get_if<EncodableMap>(&operation_value);
      if (operation == nullptr) {
        return result(FlutterError("invalid-query",
                                   "simple_query: each batch operation must be a map."));
      }
      EncodableMap merged = *operation;
      if (FindValue(*operation, "platformData") == nullptr &&
          shared_platform_data != nullptr) {
        merged[EncodableValue("platformData")] = *shared_platform_data;
      }
      ResolvedRoot root = ResolveRoot(merged);
      batch.push_back({.request = std::move(merged), .root = std::move(root)});
    }
    RunInBackground<EncodableMap>(
        [batch = std::move(batch)](const StopRequested& stop_requested) {
          return RunBatch(batch, stop_requested);
        },
        std::move(result));
  }

  // Purpose: Start polling one domain and sending its changes to Dart. The
  // first snapshot is taken before replying, so the observer reports every
  // change made after the reply.
  // @param request names the domain, the polling interval and the root.
  // @param result receives the new observer's id; invalid-query for a
  // malformed request; permission-denied, with the permission id, when
  // access is off; or unavailable when the records cannot be read or the
  // worker cannot start.
  // @returns Nothing.
  // @throws Nothing.
  void ObserveStart(const EncodableMap& request,
                    std::function<void(ErrorOr<std::string> reply)> result) override {
    std::variant<ObserverSpec, FlutterError> spec = ObserverSpecFor(request);
    if (auto* invalid = std::get_if<FlutterError>(&spec)) {
      return result(*invalid);
    }
    const ObserverSpec& observer_spec = std::get<ObserverSpec>(spec);
    RunInBackground<BaselineOrError, std::string>(
        {.work = [observer_spec](const StopRequested& stop_requested) {
           return TakeBaseline(observer_spec, stop_requested);
         },
         .finish = [this, observer_spec](BaselineOrError baseline) {
           return StartObserver(observer_spec, std::move(baseline));
         },
         .result = std::move(result)});
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
  // @param result receives a handle, the file's UTF-8 path, MIME type and
  // size, or an error.
  // @returns Nothing.
  // @throws Nothing.
  void OpenBinary(const EncodableMap& request,
                  std::function<void(ErrorOr<EncodableMap> reply)> result) override {
    const std::optional<std::string> domain = StringAt(request, "domain");
    if (!domain.has_value()) {
      return result(MissingDomainError());
    }
    if (!IsFileDomain(*domain)) {
      return result(NotSupportedError("openBinary", *domain));
    }
    const std::optional<std::string> path_text = BinaryPathText(request);
    if (!path_text.has_value()) {
      return result(FlutterError("invalid-query",
                                 "simple_query: openBinary requires recordId or "
                                 "platformData.path. Send the file's path."));
    }
    std::filesystem::path path;
    try {
      path = PathFromUtf8(*path_text);
    } catch (const std::system_error& error) {
      return result(FlutterError("unavailable",
                                 std::string("simple_query: openBinary could not read the "
                                             "path. Check that it is valid Unicode - ") +
                                     error.what()));
    }
    RunInBackground<ErrorOr<EncodableMap>, EncodableMap>(
        {.work = [path](const StopRequested&) { return DescribeBinaryFile(path); },
         .finish = [this, path](ErrorOr<EncodableMap> described) {
           return RegisterHandle(path, std::move(described));
         },
         .result = std::move(result)});
  }

  // Purpose: Forget a handle OpenBinary returned.
  // @param handle_id is the handle to forget.
  // @returns Nothing on success.
  // @throws Nothing.
  std::optional<FlutterError> CloseBinary(const std::string& handle_id) override {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  // Purpose: Run one Windows-specific extension method. Store listings run
  // on the background worker; the rest answer at once.
  // @param extension_namespace is the extension namespace, such as
  // windows.storage.
  // @param method is the method name.
  // @param args are the method's arguments, or null.
  // @param result receives the method's result, or an error for an unknown
  // method, invalid arguments, a denied store or an unreadable store.
  // @returns Nothing.
  // @throws Nothing.
  void CallExtension(
      const std::string& extension_namespace, const std::string& method,
      const EncodableMap* args,
      std::function<void(ErrorOr<std::optional<EncodableMap>> reply)> result) override {
    const std::string qualified = extension_namespace + "." + method;
    const bool lists_stores = qualified == "windows.contacts.listStores" ||
                              qualified == "windows.calendar.listCalendars";
    if (lists_stores) {
      if (auto rejected = RejectArguments(args, qualified)) {
        return result(*rejected);
      }
      const bool contacts = qualified == "windows.contacts.listStores";
      return RunInBackground<std::optional<EncodableMap>>(
          [contacts](const StopRequested& stop_requested) {
            return contacts ? StoresReply("stores", ListContactStores(stop_requested))
                            : StoresReply("calendars", ListCalendarStores(stop_requested));
          },
          std::move(result));
    }
    if (qualified == "windows.storage.resolveKnownFolders") {
      return result(ResolveKnownFolders(args, current_directory_));
    }
    if (qualified == "windows.storage.listLibraries") {
      if (auto rejected = RejectArguments(args, qualified)) {
        return result(*rejected);
      }
      return result(ListLibraries());
    }
    result(FlutterError("not-supported", "simple_query: " + qualified +
                                             " is not supported on Windows. Check "
                                             "platformExtensions from getCapabilities."));
  }

 private:
  // Purpose: Run a call's work on the background worker, then build and send
  // its reply on the platform thread. A call the worker cannot take is
  // answered at once.
  // @param call is the work, the platform-thread step and the reply.
  // @returns Nothing.
  // @throws Nothing.
  template <typename Value, typename Reply>
  void RunInBackground(BackgroundCall<Value, Reply> call) {
    std::function<void(ErrorOr<Reply>)> refused = call.result;
    BackgroundJob job = [work = std::move(call.work), finish = std::move(call.finish),
                         result = std::move(call.result)](
                            const StopRequested& stop_requested) -> std::function<void()> {
      Value value = work(stop_requested);
      return [finish, result, value = std::move(value)]() mutable {
        result(finish(std::move(value)));
      };
    };
    const EnqueueResult queued = background_calls_.Enqueue(std::move(job));
    if (queued != EnqueueResult::kQueued) {
      refused(NotQueuedError(queued));
    }
  }

  // Purpose: Run a call's work on the background worker and send its reply
  // unchanged on the platform thread.
  // @param work runs on the worker; see BackgroundCall::work.
  // @param result is Pigeon's reply callback.
  // @returns Nothing.
  // @throws Nothing.
  template <typename Reply>
  void RunInBackground(std::function<ErrorOr<Reply>(const StopRequested&)> work,
                       std::function<void(ErrorOr<Reply>)> result) {
    RunInBackground<ErrorOr<Reply>, Reply>(
        {.work = std::move(work),
         .finish = [](ErrorOr<Reply> reply) { return reply; },
         .result = std::move(result)});
  }

  // Purpose: Resolve a files or media request's root on the platform thread.
  // @param request may name the root in platformData.rootPath.
  // @returns The root, or why it could not be resolved.
  // @throws Nothing.
  ResolvedRoot ResolveRoot(const EncodableMap& request) {
    try {
      return {.path = ResolveRootPath(request, current_directory_), .error = std::nullopt};
    } catch (const std::system_error& error) {
      return {.path = std::nullopt, .error = error};
    }
  }

  // Purpose: Validate an observe request and build its observer spec.
  // @param request names the domain, the polling interval and the root.
  // @returns The spec, or the error Dart receives.
  // @throws Nothing.
  std::variant<ObserverSpec, FlutterError> ObserverSpecFor(const EncodableMap& request) {
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
      ResolvedRoot resolved = ResolveRoot(request);
      if (resolved.error.has_value()) {
        return FlutterError("unavailable",
                            std::string("simple_query: windows observe could not "
                                        "resolve its root directory. Check "
                                        "platformData.rootPath - ") +
                                resolved.error->what());
      }
      root = std::move(*resolved.path);
    }
    return ObserverSpec{
        .domain = *domain,
        .polling_interval = std::get<std::chrono::milliseconds>(interval),
        .take_snapshot =
            [root, domain = *domain](const StopRequested& stop_requested) {
              return BuildSnapshotForDomain(
                  {.root = root, .domain = domain, .stop_requested = stop_requested});
            },
    };
  }

  // Purpose: Start an observer from its first snapshot. Runs on the
  // platform thread.
  // @param spec is the observer's spec.
  // @param baseline is the first snapshot, or the error it failed with.
  // @returns The observer's id, or the error Dart receives.
  // @throws Nothing.
  ErrorOr<std::string> StartObserver(const ObserverSpec& spec, BaselineOrError baseline) {
    if (auto* error = std::get_if<FlutterError>(&baseline)) {
      return *error;
    }
    try {
      return observers_.Start(spec, std::get<Snapshot>(std::move(baseline)));
    } catch (const std::system_error& error) {
      return FlutterError("unavailable",
                          std::string("simple_query: windows observe could not "
                                      "start its polling thread. Try again - ") +
                              error.what());
    }
  }

  // Purpose: Give a described file a handle Dart can close. Runs on the
  // platform thread.
  // @param path is the file's path.
  // @param described is the file's description, or the error it failed with.
  // @returns The description with its handle, or the error.
  // @throws Nothing.
  ErrorOr<EncodableMap> RegisterHandle(const std::filesystem::path& path,
                                       ErrorOr<EncodableMap> described) {
    if (described.has_error()) {
      return described;
    }
    EncodableMap result = described.value();
    const std::string handle = "win_handle_" + std::to_string(++handle_counter_);
    open_handles_[handle] = path;
    result[EncodableValue("handleId")] = EncodableValue(handle);
    return result;
  }

  // Read only on the platform thread.
  const std::function<std::filesystem::path()> current_directory_;
  ObserveEventSender event_sender_;
  int64_t handle_counter_ = 0;
  std::map<std::string, std::filesystem::path> open_handles_;
  // Declared after event_sender_ so it is destroyed first: no worker can
  // wake it once it is gone, and no event is sent after the sender goes.
  PollingObservers observers_;
  // Declared last so it is destroyed first: its pending replies use this
  // object, and are dropped before any member they touch goes away.
  BackgroundCalls background_calls_;
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
