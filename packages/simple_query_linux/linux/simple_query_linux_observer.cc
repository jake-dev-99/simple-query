#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <set>
#include <thread>
#include <utility>

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
 * @returns A timestamp with fixed millisecond precision and a literal Z suffix.
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

/** Purpose: Own one observer's worker, cancellation, and queued deliveries.
 * Ownership: The registry and asynchronous deliveries share this state; its
 * cancellable is released automatically if startup never reaches the registry.
 */
struct ObserverState {
  std::atomic<bool> active{true};
  std::atomic<bool> delivery_pending{false};
  std::thread worker;
  std::condition_variable wakeup;
  std::mutex wakeup_mutex;
  std::mutex sources_mutex;
  std::set<GSource*> pending_sources;
  CancellablePtr cancellable;
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
 * @returns Nothing.
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
  const std::string observer_id =
      std::string("linux_observer_") + std::to_string(++observer_counter_);
  std::string copy_error;
  auto request_copy = CopyValueForWorker(request, &copy_error);
  if (request_copy == nullptr) {
    return StringResult{"", NativeError{"unavailable", copy_error}};
  }
  StringResult response{observer_id, std::nullopt};
  auto state = std::make_shared<ObserverState>();
  state->cancellable.reset(g_cancellable_new());
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.emplace(observer_id, state);
  }

  try {
    state->worker = std::thread([this, observer_id, domain,
                                 request = std::move(request_copy), interval_ms,
                                 state]() mutable {
      RunObserver(observer_id, domain, std::move(request), interval_ms, state);
    });
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      observers_.erase(observer_id);
    }
    StopObserver(state);
    throw;
  }
  return response;
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

/** Purpose: Own the state needed to finish one asynchronous FlutterApi send. */
struct NativeQueryHostApiImpl::ObserveCompletion {
  std::shared_ptr<ObserverState> state;
  std::string observer_id;
};

/** Purpose: Own one platform-context source, event, and preallocated callback.
 */
struct NativeQueryHostApiImpl::ObserveDelivery {
  NativeQueryHostApiImpl* host;
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
      delivery->host->flutter_api_, delivery->observer_id.c_str(),
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
    auto delivery = std::make_unique<ObserveDelivery>(
        ObserveDelivery{this, state, observer_id, std::move(event),
                        source.get(), std::move(completion)});
    g_source_set_callback(source.get(), DeliverObserveEvent, delivery.release(),
                          DestroyObserveDelivery);
    {
      std::lock_guard<std::mutex> lock(state->sources_mutex);
      if (!state->active.load()) {
        return false;
      }
      state->pending_sources.insert(source.get());
      g_source_attach(source.get(), platform_context_);
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
 * @param state owns cancellation and lifecycle coordination.
 * @returns Nothing.
 * @throws Nothing.
 */
void NativeQueryHostApiImpl::RunObserver(
    const std::string& observer_id, const std::string& domain, ValuePtr request,
    int64_t interval_ms, const std::shared_ptr<ObserverState>& state) noexcept {
  try {
    auto previous = BuildSnapshotForDomain(request.get(), domain);
    if (previous.error.has_value()) {
      g_warning("simple_query: observer %s stopped - %s", observer_id.c_str(),
                previous.error->c_str());
      state->active.store(false);
      return;
    }
    while (state->active.load()) {
      std::unique_lock<std::mutex> lock(state->wakeup_mutex);
      if (state->wakeup.wait_for(lock, std::chrono::milliseconds(interval_ms),
                                 [&state] { return !state->active.load(); })) {
        break;
      }
      lock.unlock();
      auto current = BuildSnapshotForDomain(request.get(), domain);
      if (current.error.has_value()) {
        g_warning("simple_query: observer %s stopped - %s", observer_id.c_str(),
                  current.error->c_str());
        state->active.store(false);
        return;
      }
      if (current.snapshot == previous.snapshot) {
        continue;
      }
      auto event = Value(fl_value_new_map());
      MapSetString(event.get(), "domain", domain);
      MapSetString(event.get(), "changeType", "unknown");
      MapSetString(event.get(), "timestamp", IsoUtcTimestamp());
      MapSet(event.get(), "ids",
             ChangedIds(previous.snapshot, current.snapshot).release());
      MapSetString(event.get(), "source", "linux-host");
      if (QueueObserveEvent(observer_id, state, std::move(event))) {
        previous = std::move(current);
      }
    }
  } catch (const std::exception& error) {
    g_warning("simple_query: observer %s stopped - %s", observer_id.c_str(),
              error.what());
    state->active.store(false);
  } catch (...) {
    g_warning("simple_query: observer %s stopped - unknown native failure",
              observer_id.c_str());
    state->active.store(false);
  }
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
  if (state->worker.joinable()) {
    state->worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(state->sources_mutex);
    for (GSource* source : state->pending_sources) {
      g_source_destroy(source);
      g_source_unref(source);
    }
    state->pending_sources.clear();
  }
  if (state->cancellable != nullptr) {
    g_cancellable_cancel(state->cancellable.get());
    state->cancellable.reset();
  }
}

/**
 * Purpose: Apply StopObserver to every observer during plugin disposal.
 * @returns Nothing.
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
