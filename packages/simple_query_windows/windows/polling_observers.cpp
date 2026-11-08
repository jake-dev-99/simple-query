#include "polling_observers.h"

#include "structured_log.h"

#include <cstddef>
#include <exception>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace simple_query_windows {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

// Purpose: Hash encodable values field by field with 64-bit FNV-1a, so a
// fingerprint needs no encoded copy of the row.
class RowHasher {
 public:
  // Purpose: Add a map's size, then each key and value in key order.
  // @param map is the map to add.
  // @returns Nothing.
  // @throws std::invalid_argument for a custom value, which has no bytes to
  // hash; std::logic_error for a value type this hasher does not know.
  void AddMap(const EncodableMap& map) {
    AddSize(map.size());
    for (const auto& [key, item] : map) {
      Add(key);
      Add(item);
    }
  }

  // Purpose: Add one value, its type first, so equal bytes of different
  // types hash differently.
  // @param value is the value to add.
  // @returns Nothing.
  // @throws std::invalid_argument for a custom value, which has no bytes to
  // hash; std::logic_error for a value type this hasher does not know.
  void Add(const EncodableValue& value) {
    AddSize(value.index());
    if (const auto* flag = std::get_if<bool>(&value)) {
      AddScalar(*flag);
    } else if (const auto* int_value = std::get_if<int32_t>(&value)) {
      AddScalar(*int_value);
    } else if (const auto* long_value = std::get_if<int64_t>(&value)) {
      AddScalar(*long_value);
    } else if (const auto* number = std::get_if<double>(&value)) {
      AddScalar(*number);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      AddSize(text->size());
      AddBytes(text->data(), text->size());
    } else if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
      AddArray(*bytes);
    } else if (const auto* ints = std::get_if<std::vector<int32_t>>(&value)) {
      AddArray(*ints);
    } else if (const auto* longs = std::get_if<std::vector<int64_t>>(&value)) {
      AddArray(*longs);
    } else if (const auto* floats = std::get_if<std::vector<float>>(&value)) {
      AddArray(*floats);
    } else if (const auto* doubles = std::get_if<std::vector<double>>(&value)) {
      AddArray(*doubles);
    } else if (const auto* list = std::get_if<EncodableList>(&value)) {
      AddSize(list->size());
      for (const auto& item : *list) {
        Add(item);
      }
    } else if (const auto* map = std::get_if<EncodableMap>(&value)) {
      AddMap(*map);
    } else if (std::holds_alternative<flutter::CustomEncodableValue>(value)) {
      throw std::invalid_argument(
          "simple_query_windows: a record fingerprint cannot hash a custom value");
    } else if (!std::holds_alternative<std::monostate>(value)) {
      throw std::logic_error(
          "simple_query_windows: a record fingerprint met an unhandled value type");
    }
    // A null value is fully described by its type, added above.
  }

  // Purpose: Give the hash of everything added so far.
  // @returns The hash.
  // @throws Nothing.
  uint64_t Result() const { return hash_; }

 private:
  // Purpose: Add raw bytes.
  // @param data points at the bytes.
  // @param size is the number of bytes.
  // @returns Nothing.
  // @throws Nothing.
  void AddBytes(const void* data, size_t size) {
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
      hash_ ^= bytes[index];
      hash_ *= kFnvPrime;
    }
  }

  // Purpose: Add a length, so adjacent values cannot run together.
  // @param size is the length.
  // @returns Nothing.
  // @throws Nothing.
  void AddSize(size_t size) { AddScalar(static_cast<uint64_t>(size)); }

  // Purpose: Add one plain value's bytes.
  // @param value is the value.
  // @returns Nothing.
  // @throws Nothing.
  template <typename Scalar>
  void AddScalar(Scalar value) {
    AddBytes(&value, sizeof(value));
  }

  // Purpose: Add an array's length, then its elements' bytes.
  // @param items are the elements.
  // @returns Nothing.
  // @throws Nothing.
  template <typename Item>
  void AddArray(const std::vector<Item>& items) {
    AddSize(items.size());
    if (!items.empty()) {
      AddBytes(items.data(), items.size() * sizeof(Item));
    }
  }

  // The 64-bit FNV-1a offset basis.
  uint64_t hash_ = 14695981039346656037ULL;
};

}  // namespace

// Purpose: Hold one observer's worker thread and how it is told to stop.
struct PollingObservers::WorkerLifecycle {
  // Read without the observer's mutex by snapshot scans; written under it.
  std::atomic<bool> stopping{false};
  // Wakes the worker early when the observer is stopping.
  std::condition_variable stop_signal;
  std::thread worker;
};

// Purpose: Hold what one observer last delivered and what waits to go.
struct PollingObservers::DeliveryState {
  // The snapshot the last delivered event described.
  Snapshot delivered;
  // The change waiting for delivery, if any.
  std::optional<PendingChange> pending;
};

// Purpose: Hold one observer's settings, worker and change state.
struct PollingObservers::Observer {
  // Purpose: Hold the settings for one observer before its worker starts.
  // @param observer_spec describes what to poll and how often.
  // @returns Nothing.
  // @throws Nothing.
  explicit Observer(ObserverSpec observer_spec)
      : spec(std::move(observer_spec)),
        stop_requested([this]() { return lifecycle.stopping.load(); }) {}

  const ObserverSpec spec;
  // Given to each snapshot so a long scan ends once the observer stops.
  const StopRequested stop_requested;
  // Guards delivery, and orders a stop request with the worker's wait.
  std::mutex mutex;
  WorkerLifecycle lifecycle;
  // Guarded by mutex.
  DeliveryState delivery;
};

// Purpose: Prepare to deliver events through effects.send_event.
// @param effects supplies event delivery and the clock.
// @returns Nothing; check IsReady() before starting observers.
// @throws Nothing; a failed setup leaves IsReady() false.
PollingObservers::PollingObservers(PollingObserverEffects effects)
    : effects_(std::move(effects)),
      dispatcher_([this]() { DeliverPendingChanges(); }) {}

// Purpose: Stop every observer, then release delivery resources.
// Changes not yet delivered are dropped.
// @returns Nothing.
// @throws Nothing.
PollingObservers::~PollingObservers() {
  std::vector<std::unique_ptr<Observer>> stopping;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    for (auto& entry : observers_) {
      stopping.push_back(std::move(entry.second));
    }
    observers_.clear();
  }
  // Signal every worker before joining any, so teardown waits for the
  // slowest worker once instead of for each worker in turn.
  for (auto& observer : stopping) {
    RequestStop(*observer);
  }
  for (auto& observer : stopping) {
    JoinWorker(*observer);
  }
}

// Purpose: Report whether events can reach the platform thread.
// @returns true when observers can be started.
// @throws Nothing.
bool PollingObservers::IsReady() const { return dispatcher_.IsReady(); }

// Purpose: Start polling one domain on a new worker thread.
// @param spec describes what to poll and how often.
// @returns The new observer's id, used in its events and to stop it.
// @throws std::system_error when the worker thread cannot be started.
std::string PollingObservers::Start(ObserverSpec spec) {
  auto observer = std::make_unique<Observer>(std::move(spec));
  Observer* worker_observer = observer.get();
  std::string observer_id;
  {
    // Stored before its worker starts, so a failed insert never leaves a
    // running worker that nothing can stop.
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observer_id = "windows_observer_" + std::to_string(++observer_counter_);
    observers_[observer_id] = std::move(observer);
  }
  try {
    worker_observer->lifecycle.worker =
        std::thread([this, worker_observer]() { RunWorker(worker_observer); });
  } catch (const std::system_error&) {
    // No worker started, so the observer is removed and the caller told.
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.erase(observer_id);
    throw;
  }
  return observer_id;
}

// Purpose: Stop one observer and wait for its worker to finish. Its
// undelivered change is dropped. An unknown id does nothing.
// @param observer_id is the id Start returned.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::Stop(const std::string& observer_id) {
  std::unique_ptr<Observer> observer;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    auto it = observers_.find(observer_id);
    if (it == observers_.end()) {
      return;
    }
    observer = std::move(it->second);
    observers_.erase(it);
  }
  RequestStop(*observer);
  JoinWorker(*observer);
}

// Purpose: Poll one observer until it is stopped.
// @param observer is the observer this worker serves; it outlives the
// worker, which is always joined before the observer is destroyed.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::RunWorker(Observer* observer) {
  FailureStreaks streaks;
  // Without a first snapshot there is nothing to compare against, and an
  // empty baseline would report every record as added.
  std::optional<Snapshot> baseline = TakeSnapshot(observer, &streaks);
  while (!baseline.has_value()) {
    if (WaitForNextPoll(observer)) {
      return;
    }
    baseline = TakeSnapshot(observer, &streaks);
  }
  {
    std::lock_guard<std::mutex> lock(observer->mutex);
    observer->delivery.delivered = std::move(*baseline);
  }

  while (!WaitForNextPoll(observer)) {
    std::optional<Snapshot> current = TakeSnapshot(observer, &streaks);
    if (current.has_value() && RecordSnapshot(observer, std::move(*current))) {
      RequestDelivery(&streaks);
    }
  }
}

// Purpose: Take one snapshot for a worker, logging a failure once per
// streak.
// @param observer is the observer being polled.
// @param streaks records which failures this worker has logged.
// @returns The snapshot, or nothing when it failed or the observer is
// stopping.
// @throws Nothing.
std::optional<Snapshot> PollingObservers::TakeSnapshot(Observer* observer,
                                                       FailureStreaks* streaks) {
  std::optional<Snapshot> snapshot;
  std::string failure;
  try {
    snapshot = observer->spec.take_snapshot(observer->stop_requested);
  } catch (const std::exception& error) {
    // An exception leaving a worker thread ends the app, so the worker is
    // the nearest place that can handle a failed read: the poll is skipped
    // and the next one tries again.
    failure = error.what();
  }
  if (observer->lifecycle.stopping.load()) {
    return std::nullopt;
  }
  if (snapshot.has_value()) {
    streaks->snapshot_failing = false;
    return snapshot;
  }
  if (!streaks->snapshot_failing) {
    streaks->snapshot_failing = true;
    WriteLog({LogLevel::kError,
              "observe_snapshot_failed",
              {{"domain", observer->spec.domain},
               {"error", failure},
               {"retry", "each poll"},
               {"next_log", "after a snapshot succeeds"}}});
  }
  return std::nullopt;
}

// Purpose: Wait one polling interval, or less when the observer stops.
// @param observer is the observer being polled.
// @returns true when the observer is stopping.
// @throws Nothing.
bool PollingObservers::WaitForNextPoll(Observer* observer) {
  std::unique_lock<std::mutex> lock(observer->mutex);
  return observer->lifecycle.stop_signal.wait_for(
      lock, observer->spec.polling_interval,
      [observer]() { return observer->lifecycle.stopping.load(); });
}

// Purpose: Ask the platform thread to deliver pending changes, logging a
// failure once per streak.
// @param streaks records which failures this worker has logged.
// @returns Nothing; a pending change whose wake failed is retried after
// the next poll.
// @throws Nothing.
void PollingObservers::RequestDelivery(FailureStreaks* streaks) {
  if (dispatcher_.RequestWake()) {
    streaks->wake_failing = false;
    return;
  }
  // Read before logging, which can change the thread's last error.
  const DWORD error = GetLastError();
  if (streaks->wake_failing) {
    return;
  }
  streaks->wake_failing = true;
  // The pending change stays recorded, so the next poll asks again.
  WriteLog({LogLevel::kError,
            "observe_wake_failed",
            {{"windows_error", std::to_string(error)},
             {"retry", "after each poll"},
             {"next_log", "after an event is scheduled"}}});
}

// Purpose: Store a new snapshot as the observer's pending change.
// @param observer is the observer that took the snapshot.
// @param current is the snapshot just taken.
// @returns true when a change is pending and needs delivery.
// @throws Nothing.
bool PollingObservers::RecordSnapshot(Observer* observer, Snapshot current) {
  std::lock_guard<std::mutex> lock(observer->mutex);
  if (current == observer->delivery.delivered) {
    // Back to what was last delivered: nothing is left to report.
    observer->delivery.pending.reset();
    return false;
  }
  const bool already_pending = observer->delivery.pending.has_value() &&
                               observer->delivery.pending->snapshot == current;
  if (!already_pending) {
    observer->delivery.pending = PendingChange{ChangedIds(observer->delivery.delivered, current),
                                      std::move(current),
                                      effects_.now_epoch_ms()};
  }
  return true;
}

// Purpose: Deliver every pending change. Runs on the platform thread.
// @returns Nothing.
// @throws Nothing beyond what effects.send_event throws.
void PollingObservers::DeliverPendingChanges() {
  std::vector<std::pair<std::string, EncodableMap>> events;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    for (auto& [observer_id, observer] : observers_) {
      std::lock_guard<std::mutex> observer_lock(observer->mutex);
      if (!observer->delivery.pending.has_value()) {
        continue;
      }
      events.emplace_back(observer_id,
                          BuildEvent(observer->spec.domain, *observer->delivery.pending));
      observer->delivery.delivered = std::move(observer->delivery.pending->snapshot);
      observer->delivery.pending.reset();
    }
  }
  // Send outside the locks; Stop runs on this same thread, so no observer
  // can stop between collecting its event and sending it.
  for (const auto& [observer_id, event] : events) {
    effects_.send_event(observer_id, event);
  }
}

// Purpose: Build the event map sent for one change.
// @param domain is the observed domain.
// @param change is the change being delivered.
// @returns The event map.
// @throws Nothing.
EncodableMap PollingObservers::BuildEvent(const std::string& domain,
                                          const PendingChange& change) {
  EncodableMap event;
  event[EncodableValue("domain")] = EncodableValue(domain);
  event[EncodableValue("changeType")] = EncodableValue("unknown");
  event[EncodableValue("timestamp")] =
      EncodableValue(std::to_string(change.detected_at_epoch_ms));
  event[EncodableValue("ids")] = EncodableValue(change.ids);
  event[EncodableValue("source")] = EncodableValue("windows-host");
  return event;
}

// Purpose: Wake an observer's worker and tell it to finish.
// @param observer is the observer to stop.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::RequestStop(Observer& observer) {
  {
    // Set under the mutex so a worker between its stop check and its wait
    // cannot miss the signal.
    std::lock_guard<std::mutex> lock(observer.mutex);
    observer.lifecycle.stopping.store(true);
  }
  observer.lifecycle.stop_signal.notify_all();
}

// Purpose: Wait for an observer's worker to finish.
// @param observer is an observer whose stop was already requested.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::JoinWorker(Observer& observer) {
  if (observer.lifecycle.worker.joinable()) {
    observer.lifecycle.worker.join();
  }
}

// Purpose: List the ids whose values differ between two snapshots, including
// ids present in only one of them.
// @param before is the earlier snapshot.
// @param after is the later snapshot.
// @returns The differing ids, in ascending order.
// @throws Nothing.
EncodableList ChangedIds(const Snapshot& before, const Snapshot& after) {
  std::set<std::string> ids;
  for (const auto& [id, value] : before) {
    const auto match = after.find(id);
    if (match == after.end() || match->second != value) {
      ids.insert(id);
    }
  }
  for (const auto& entry : after) {
    if (before.find(entry.first) == before.end()) {
      ids.insert(entry.first);
    }
  }

  EncodableList values;
  for (const auto& id : ids) {
    values.push_back(EncodableValue(id));
  }
  return values;
}

// Purpose: Build a snapshot that changes whenever any field of a row changes.
// @param rows are the records, each with a string "id" field.
// @returns Each id mapped to a hash of its whole row. Rows without a string id
// are left out.
// @throws std::invalid_argument when a row holds a custom value, which has no
// bytes to hash; std::logic_error for a value type the hash does not know.
Snapshot FingerprintRecords(const std::vector<EncodableMap>& rows) {
  Snapshot snapshot;
  for (const auto& row : rows) {
    const auto id = row.find(EncodableValue("id"));
    if (id == row.end()) {
      continue;
    }
    const auto* id_text = std::get_if<std::string>(&id->second);
    if (id_text == nullptr) {
      continue;
    }
    // The map's keys are ordered, so equal rows always hash alike.
    RowHasher hasher;
    hasher.AddMap(row);
    snapshot[*id_text] = static_cast<int64_t>(hasher.Result());
  }
  return snapshot;
}

}  // namespace simple_query_windows
