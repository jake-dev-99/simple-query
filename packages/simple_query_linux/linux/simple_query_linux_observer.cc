#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <set>
#include <utility>
#include <vector>

#include "simple_query_linux_plugin_private.h"

namespace simple_query_linux {

/**
 * Purpose: Give an observer thread a fully independent FlValue because
 * Flutter Linux FlValue reference counts are not atomic.
 * @param value is encoded and decoded on the platform thread.
 * @param error_message receives a stable simple_query failure on codec error.
 * @returns A deep copy safe for exclusive worker-thread ownership.
 * @throws std::bad_alloc if the returned string cannot be allocated.
 */
ValuePtr CopyValueForWorker(FlValue* value, std::string* error_message) {
  g_autoptr(FlStandardMessageCodec) codec = fl_standard_message_codec_new();
  g_autoptr(GError) error = nullptr;
  g_autoptr(GBytes) encoded =
      fl_message_codec_encode_message(FL_MESSAGE_CODEC(codec), value, &error);
  if (encoded == nullptr) {
    *error_message =
        std::string("simple_query: could not copy observation request - ") +
        (error != nullptr ? error->message : "encoding failed");
    return ValuePtr();
  }
  FlValue* copy =
      fl_message_codec_decode_message(FL_MESSAGE_CODEC(codec), encoded, &error);
  if (copy == nullptr) {
    *error_message =
        std::string("simple_query: could not copy observation request - ") +
        (error != nullptr ? error->message : "decoding failed");
    return ValuePtr();
  }
  return Value(copy);
}

/**
 * Purpose: Encode observation time in the ISO-8601 UTC format Dart requires.
 * @param None. @returns A timestamp with fixed millisecond precision and a literal Z suffix.
 * @throws std::bad_alloc if the returned string cannot be allocated.
 */
std::string IsoUtcTimestamp() {
  g_autoptr(GDateTime) now = g_date_time_new_now_utc();
  g_autofree gchar* prefix = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
  g_autofree gchar* suffix =
      g_strdup_printf(".%03dZ", g_date_time_get_microsecond(now) / 1000);
  return std::string(prefix) + suffix;
}

/** Purpose: Identify records added, removed, or changed between snapshots.
 * @param previous is the last delivered snapshot. @param current is the newest
 * snapshot. @returns A new Flutter list of changed IDs. @throws Nothing. */
ValuePtr ChangedIds(const Snapshot& previous, const Snapshot& current) {
  auto ids = Value(fl_value_new_list());
  for (const auto& [id, modified] : previous) {
    const auto found = current.find(id);
    if (found == current.end() || found->second != modified) {
      fl_value_append_take(ids.get(), fl_value_new_string(id.c_str()));
    }
  }
  for (const auto& [id, ignored] : current) {
    if (previous.find(id) == previous.end()) {
      fl_value_append_take(ids.get(), fl_value_new_string(id.c_str()));
    }
  }
  return ids;
}

/** Purpose: Release worker cancellation tokens through their GLib ownership API.
 * @param cancellable is supplied when an owned token leaves scope.
 * @returns A stateless deletion policy for CancellablePtr.
 * @throws Nothing. */
struct CancellableDeleter {
  /** Purpose: Release an exclusively owned GLib cancellation token.
   * @param cancellable is the nullable token leaving scope.
   * @returns Nothing. @throws Nothing. */
  void operator()(GCancellable* cancellable) const {
    if (cancellable != nullptr) {
      g_object_unref(cancellable);
    }
  }
};

using CancellablePtr = std::unique_ptr<GCancellable, CancellableDeleter>;

/** Purpose: Balance a queued source reference even when delivery is cancelled.
 * @param source is supplied when an owned source leaves scope.
 * @returns A stateless deletion policy for SourcePtr.
 * @throws Nothing. */
struct SourceDeleter {
  /** Purpose: Release an exclusively owned GLib source reference.
   * @param source is the nullable source leaving scope.
   * @returns Nothing. @throws Nothing. */
  void operator()(GSource* source) const {
    if (source != nullptr) {
      g_source_unref(source);
    }
  }
};

using SourcePtr = std::unique_ptr<GSource, SourceDeleter>;

/** Purpose: Keep worker resources alive without retaining the plugin host.
 * @param api supplies retained Flutter transport for asynchronous delivery.
 * @param context supplies the platform context required for final teardown.
 * @returns Shared cancellation, delivery, and platform-affine ownership state.
 * @throws Nothing during construction; shared allocation occurs in the factory. */
struct ObserverState {
  /**
   * Purpose: Retain transport resources on the platform thread.
   * @param api is the generated Flutter API retained for asynchronous sends.
   * @param context is the platform context that owns final resource teardown.
   * @returns A state whose references are released on the platform thread.
   * @throws Nothing.
   */
  ObserverState(SqlqNativeQueryFlutterApi* api, GMainContext* context)
      : cancellable(g_cancellable_new()),
        flutter_api(SQLQ_NATIVE_QUERY_FLUTTER_API(g_object_ref(api))),
        platform_context(g_main_context_ref(context)),
        platform_thread(g_thread_self()) {}

/**
   * Purpose: Release platform-affine references after all worker work ends.
   * @param None. @returns Nothing.
   * @throws Nothing.
   */
  ~ObserverState() {
    g_assert_true(g_thread_self() == platform_thread);
    g_clear_object(&flutter_api);
    cancellable.reset();
    g_clear_pointer(&platform_context, g_main_context_unref);
  }

  /** Purpose: Prevent duplicate ownership of platform-affine references.
   * @param other is intentionally not copied.
   * @returns Nothing because construction is disabled.
   * @throws Nothing. */
  ObserverState(const ObserverState& other) = delete;
  /** Purpose: Prevent duplicate assignment of platform-affine references.
   * @param other is intentionally not assigned.
   * @returns Nothing because assignment is disabled.
   * @throws Nothing. */
  ObserverState& operator=(const ObserverState& other) = delete;

  std::atomic<bool> active{true};
  std::atomic<bool> delivery_pending{false};
  std::condition_variable wakeup;
  std::mutex wakeup_mutex;
  std::mutex sources_mutex;
  std::set<GSource*> pending_sources;
  CancellablePtr cancellable;
  SqlqNativeQueryFlutterApi* flutter_api;
  GMainContext* platform_context;
  GThread* platform_thread;
};

/**
 * Purpose: Delete observer state only when its captured platform context runs.
 * @param user_data is the state whose last shared owner exited on a worker.
 * @returns G_SOURCE_REMOVE after releasing the state.
 * @throws Nothing.
 */
gboolean DeleteObserverStateOnPlatform(gpointer user_data) {
  delete static_cast<ObserverState*>(user_data);
  return G_SOURCE_REMOVE;
}

/** Purpose: Prevent the last worker owner from destroying platform resources.
 * @param state is supplied when the last shared observer owner leaves scope.
 * @returns A deletion policy that marshals teardown to the captured context.
 * @throws Nothing. */
struct ObserverStateDeleter {
  /**
   * Purpose: Prevent final Flutter or GLib object teardown on a worker thread.
   * @param state is the exclusively owned state leaving shared ownership.
   * @returns Nothing.
   * @throws Nothing.
   */
  void operator()(ObserverState* state) const noexcept {
    if (state == nullptr) {
      return;
    }
    if (g_thread_self() == state->platform_thread) {
      delete state;
      return;
    }
    GSource* source = g_idle_source_new();
    g_source_set_callback(source, DeleteObserverStateOnPlatform, state,
                          nullptr);
    g_source_attach(source, state->platform_context);
    g_source_unref(source);
  }
};

/**
 * Purpose: Create observer state with a platform-affine shared deleter.
 * @param api is the generated API used to send observation events.
 * @param context is the captured platform event-loop context.
 * @returns Shared observer state safe to release from a worker.
 * @throws std::bad_alloc when state allocation fails.
 */
std::shared_ptr<ObserverState> MakeObserverState(
    SqlqNativeQueryFlutterApi* api, GMainContext* context) {
  return std::shared_ptr<ObserverState>(new ObserverState(api, context),
                                        ObserverStateDeleter());
}

/** Purpose: Transfer polling inputs into a worker without borrowing host state.
 * @param observer_id and domain identify the observer and acquisition source.
 * @param request owns its worker-exclusive decoded query payload.
 * @param interval_ms and previous supply cadence and the validated baseline.
 * @param state retains independently cancellable transport ownership.
 * @returns Move-only inputs owned by the detached worker after startup.
 * @throws std::bad_alloc when owned identifiers or snapshots are allocated. */
struct NativeQueryHostApiImpl::ObserverWork {
  std::string observer_id;
  std::string domain;
  ValuePtr request;
  int64_t interval_ms;
  Snapshot previous;
  std::shared_ptr<ObserverState> state;
};

/**
 * Purpose: Bind Linux host behavior to Dart and capture the platform GLib
 * context used for all future Flutter engine interaction.
 * @param messenger is the registrar-owned Flutter binary messenger.
 * @returns A host bound to the current platform context.
 * @throws Nothing.
 */
NativeQueryHostApiImpl::NativeQueryHostApiImpl(FlBinaryMessenger* messenger)
    : flutter_api_(sqlq_native_query_flutter_api_new(messenger, nullptr)),
      platform_context_(g_main_context_ref_thread_default()) {}

/**
 * Purpose: Stop workers, cancel deliveries, and release platform resources.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
NativeQueryHostApiImpl::~NativeQueryHostApiImpl() {
  ShutdownObservers();
  g_clear_object(&flutter_api_);
  g_clear_pointer(&platform_context_, g_main_context_unref);
}

/** Purpose: Start one cancellable polling worker for a supported domain.
 * @param request is the decoded observation payload. @returns Its observer ID
 * or a structured error. @throws Thread or allocation exceptions for the
 * outer callback boundary to translate. */
StringResult NativeQueryHostApiImpl::ObserveStart(FlValue* request) {
  const std::string domain = StringOr(request, "domain", "platformSpecific");
  if (domain != "files" && domain != "media" && domain != "contacts" &&
      domain != "calendar") {
    return StringResult{
        "", NativeError{"not-supported",
                        "simple_query: observe is not supported for domain " +
                            domain + " on Linux host"}};
  }

  int64_t interval_ms =
      AsInt(FindValue(request, "pollingIntervalMs")).value_or(1000);
  interval_ms = std::max<int64_t>(250, interval_ms);
  std::string copy_error;
  auto request_copy = CopyValueForWorker(request, &copy_error);
  if (request_copy == nullptr) {
    return StringResult{"", NativeError{"unavailable", copy_error}};
  }
  auto state = MakeObserverState(flutter_api_, platform_context_);
  auto initial =
      BuildSnapshotForDomain(request_copy.get(), domain, state->cancellable.get());
  if (initial.error.has_value()) {
    return StringResult{"", NativeError{"unavailable", *initial.error}};
  }
  const int64_t next_observer = observer_counter_ + 1;
  const std::string observer_id =
      std::string("linux_observer_") + std::to_string(next_observer);
  auto work = std::make_unique<ObserverWork>(ObserverWork{
      observer_id, domain, std::move(request_copy), interval_ms,
      std::move(initial.snapshot), state});
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.emplace(observer_id, state);
  }
  ObserverWork* raw_work = work.release();
  g_autoptr(GError) thread_error = nullptr;
  GThread* worker = g_thread_try_new("simple-query-observer",
                                     RunObserverThread, raw_work, &thread_error);
  if (worker == nullptr) {
    delete raw_work;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      observers_.erase(observer_id);
    }
    StopObserver(state);
    return StringResult{
        "", NativeError{"unavailable",
                        std::string("simple_query: could not start observer - ") +
                            (thread_error != nullptr ? thread_error->message
                                                     : "thread creation failed")}};
  }
  g_thread_unref(worker);
  observer_counter_ = next_observer;
  return StringResult{observer_id, std::nullopt};
}

/** Purpose: Stop and remove one observer without failing repeated cleanup.
 * @param observer_id selects the observer. @returns No error after cleanup.
 * @throws Nothing after lookup succeeds. */
std::optional<NativeError> NativeQueryHostApiImpl::ObserveStop(
    const std::string& observer_id) {
  std::shared_ptr<ObserverState> state;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    const auto found = observers_.find(observer_id);
    if (found == observers_.end()) {
      return std::nullopt;
    }
    state = found->second;
    observers_.erase(found);
  }
  StopObserver(state);
  return std::nullopt;
}

/** Purpose: Keep transport completion independent of plugin-host lifetime.
 * @param state retains cancellation and platform-affine resources until finish.
 * @param observer_id identifies delivery failures in diagnostics.
 * @returns Completion context transferred to exactly one asynchronous send.
 * @throws std::bad_alloc when the diagnostic identifier is allocated. */
struct NativeQueryHostApiImpl::ObserveCompletion {
  std::shared_ptr<ObserverState> state;
  std::string observer_id;
};

/** Purpose: Own one platform-context source, event, and preallocated callback.
 * @param state and observer_id retain ownership and identify event delivery.
 * @param event owns the worker-produced payload until platform dispatch.
 * @param source identifies the queued source cancelled during observer stop.
 * @param completion owns the preallocated asynchronous finish context.
 * @param started distinguishes dispatch from destruction before dispatch.
 * @returns Move-only delivery context owned by one queued GLib source.
 * @throws std::bad_alloc when the diagnostic identifier is allocated. */
struct NativeQueryHostApiImpl::ObserveDelivery {
  std::shared_ptr<ObserverState> state;
  std::string observer_id;
  ValuePtr event;
  GSource* source;
  std::unique_ptr<ObserveCompletion> completion;
  bool started = false;
};

/**
 * Purpose: Release a queued delivery and reopen the bounded delivery slot
 * when it was destroyed before reaching Flutter.
 * @param user_data is the ObserveDelivery owned by one GLib source.
 * @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::DestroyObserveDelivery(gpointer user_data) {
  std::unique_ptr<ObserveDelivery> delivery(
      static_cast<ObserveDelivery*>(user_data));
  if (!delivery->started) {
    delivery->state->delivery_pending.store(false);
  }
}

/**
 * Purpose: Finish every generated FlutterApi send and surface Dart or
 * transport rejection without retaining the plugin host.
 * @param object is the generated FlutterApi that initiated the send.
 * @param result is the generated asynchronous result.
 * @param user_data owns observer completion context.
 * @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::FinishObserveDelivery(GObject* object,
                                                   GAsyncResult* result,
                                                   gpointer user_data) {
  std::unique_ptr<ObserveCompletion> completion(
      static_cast<ObserveCompletion*>(user_data));
  g_autoptr(GError) error = nullptr;
  g_autoptr(SqlqNativeQueryFlutterApiOnObserveEventResponse) response =
      sqlq_native_query_flutter_api_on_observe_event_finish(
          SQLQ_NATIVE_QUERY_FLUTTER_API(object), result, &error);
  if (error != nullptr &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_warning("simple_query: observer %s delivery failed - %s",
              completion->observer_id.c_str(), error->message);
  } else if (response == nullptr && error == nullptr) {
    g_warning("simple_query: observer %s delivery returned no response",
              completion->observer_id.c_str());
  } else if (response != nullptr &&
             sqlq_native_query_flutter_api_on_observe_event_response_is_error(
                 response)) {
    g_warning(
        "simple_query: observer %s delivery rejected (%s) - %s",
        completion->observer_id.c_str(),
        sqlq_native_query_flutter_api_on_observe_event_response_get_error_code(
            response),
        sqlq_native_query_flutter_api_on_observe_event_response_get_error_message(
            response));
  }
  completion->state->delivery_pending.store(false);
}

/**
 * Purpose: Run a queued observation delivery only when the platform context
 * advances, never synchronously on the polling worker.
 * @param user_data is the ObserveDelivery attached to the idle source.
 * @returns G_SOURCE_REMOVE after starting at most one FlutterApi send.
 * @throws Nothing.
 */
gboolean NativeQueryHostApiImpl::DeliverObserveEvent(gpointer user_data) {
  auto* delivery = static_cast<ObserveDelivery*>(user_data);
  {
    std::lock_guard<std::mutex> lock(delivery->state->sources_mutex);
    if (delivery->state->pending_sources.erase(delivery->source) != 0) {
      g_source_unref(delivery->source);
    }
  }
  if (!delivery->state->active.load()) {
    return G_SOURCE_REMOVE;
  }
  delivery->started = true;
  auto* completion = delivery->completion.release();
  sqlq_native_query_flutter_api_on_observe_event(
      delivery->state->flutter_api, delivery->observer_id.c_str(),
      delivery->event.get(), delivery->state->cancellable.get(),
      FinishObserveDelivery, completion);
  return G_SOURCE_REMOVE;
}

/**
 * Purpose: Bound each observer to one queued or in-flight event and attach
 * the queued work explicitly to the captured platform context.
 * @param observer_id identifies the event recipient.
 * @param state owns cancellation and pending-source tracking.
 * @param event is transferred into the queued delivery.
 * @returns True when the event was queued; false under backpressure or stop.
 * @throws Allocation exceptions for RunObserver to contain.
 */
bool NativeQueryHostApiImpl::QueueObserveEvent(
    const std::string& observer_id, const std::shared_ptr<ObserverState>& state,
    ValuePtr event) {
  bool expected = false;
  if (!state->active.load() ||
      !state->delivery_pending.compare_exchange_strong(expected, true)) {
    return false;
  }
  try {
    SourcePtr source(g_idle_source_new());
    auto completion = std::make_unique<ObserveCompletion>(
        ObserveCompletion{state, observer_id});
    auto delivery = std::make_unique<ObserveDelivery>(ObserveDelivery{
        state, observer_id, std::move(event), source.get(),
        std::move(completion)});
    g_source_set_callback(source.get(), DeliverObserveEvent, delivery.release(),
                          DestroyObserveDelivery);
    {
      std::lock_guard<std::mutex> lock(state->sources_mutex);
      if (!state->active.load()) {
        return false;
      }
      state->pending_sources.insert(source.get());
      g_source_attach(source.get(), state->platform_context);
    }
    source.release();
    return true;
  } catch (...) {
    state->delivery_pending.store(false);
    throw;
  }
}

/**
 * Purpose: Poll one native domain without letting failures escape the worker
 * or masquerade as an empty snapshot.
 * @param observer_id identifies log and callback context.
 * @param domain selects the native source.
 * @param request is exclusively owned by this worker.
 * @param interval_ms is the polling cadence.
 * @param previous is the validated initial snapshot.
 * @param state owns cancellation and lifecycle coordination.
 * @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::RunObserver(
    const std::string& observer_id, const std::string& domain, ValuePtr request,
    int64_t interval_ms, Snapshot previous,
    const std::shared_ptr<ObserverState>& state) noexcept {
  try {
    bool snapshot_failure_logged = false;
    while (state->active.load()) {
      std::unique_lock<std::mutex> lock(state->wakeup_mutex);
      if (state->wakeup.wait_for(lock, std::chrono::milliseconds(interval_ms),
                                 [&state] { return !state->active.load(); })) {
        break;
      }
      lock.unlock();
      auto current = BuildSnapshotForDomain(request.get(), domain,
                                            state->cancellable.get());
      if (current.error.has_value()) {
        if (!state->active.load()) {
          return;
        }
        if (!snapshot_failure_logged) {
          g_warning("simple_query: observer %s snapshot unavailable - %s",
                    observer_id.c_str(), current.error->c_str());
          snapshot_failure_logged = true;
        }
        continue;
      }
      snapshot_failure_logged = false;
      if (current.snapshot == previous) {
        continue;
      }
      auto event = Value(fl_value_new_map());
      MapSetString(event.get(), "domain", domain);
      MapSetString(event.get(), "changeType", "unknown");
      MapSetString(event.get(), "timestamp", IsoUtcTimestamp());
      MapSet(event.get(), "ids",
             ChangedIds(previous, current.snapshot).release());
      MapSetString(event.get(), "source", "linux-host");
      if (QueueObserveEvent(observer_id, state, std::move(event))) {
        previous = std::move(current.snapshot);
      }
    }
  } catch (const std::exception& error) {
    if (state->active.load()) {
      g_warning("simple_query: observer %s stopped - %s", observer_id.c_str(),
                error.what());
    }
    state->active.store(false);
  } catch (...) {
    if (state->active.load()) {
      g_warning("simple_query: observer %s stopped - unknown native failure",
                observer_id.c_str());
    }
    state->active.store(false);
  }
}

/**
 * Purpose: Transfer observer work into its detached GLib worker lifetime.
 * @param user_data is the ObserverWork transferred by g_thread_try_new.
 * @returns Null when polling ends.
 * @throws Nothing.
 */
gpointer NativeQueryHostApiImpl::RunObserverThread(gpointer user_data) {
  std::unique_ptr<ObserverWork> work(static_cast<ObserverWork*>(user_data));
  RunObserver(work->observer_id, work->domain, std::move(work->request),
              work->interval_ms, std::move(work->previous), work->state);
  return nullptr;
}

/**
 * Purpose: Stop one worker, destroy queued sources, and cancel in-flight
 * delivery before releasing observer state.
 * @param state is the observer removed from the public registry.
 * @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::StopObserver(
    const std::shared_ptr<ObserverState>& state) {
  state->active.store(false);
  state->wakeup.notify_all();
  if (state->cancellable != nullptr) {
    g_cancellable_cancel(state->cancellable.get());
  }
  std::set<GSource*> sources;
  {
    std::lock_guard<std::mutex> lock(state->sources_mutex);
    sources.swap(state->pending_sources);
  }
  for (GSource* source : sources) {
    g_source_destroy(source);
    g_source_unref(source);
  }
}

/**
 * Purpose: Apply StopObserver to every observer during plugin disposal.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::ShutdownObservers() {
  std::map<std::string, std::shared_ptr<ObserverState>> states;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    states.swap(observers_);
  }
  for (auto& item : states) {
    StopObserver(item.second);
  }
}

}  // namespace simple_query_linux
