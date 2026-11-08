#include "polling_observers.h"

#include "boundary_formats.h"
#include "detached_threads.h"
#include "structured_log.h"
#include "owned_message_error.h"

#include <chrono>
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
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
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
      AddScalar(static_cast<uint8_t>(*flag));
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

// Purpose: Hold what one observer's worker needs once it may outlive the
// PollingObservers that started it, and how it is told to stop.
// @param stopping is set under the observer's mutex. Snapshot scans read it
// without the mutex, so it is atomic.
// @param stop_signal is notified after stopping is set under the
// observer's mutex, so the worker cannot miss it between check and wait.
// @param dispatcher is shared with the owner, which closes it on teardown.
// @param now_epoch_ms is a copy of the owner's clock, safe to call from the
// worker.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PollingObservers::WorkerLifecycle {
  std::atomic<bool> stopping{false};
  std::condition_variable stop_signal;
  std::shared_ptr<PlatformThreadDispatcher> dispatcher;
  std::function<int64_t()> now_epoch_ms;
};

// Purpose: Hold what one observer last delivered and what waits to go.
// @param delivered is the baseline each pending change is measured
// against. It moves only when an event is sent.
// @param pending holds at most one change; a later differing poll
// replaces it.
// @param awaiting_reply is true from sending an event until Dart answers it.
// No further event is sent meanwhile, so replies cannot pile up behind a
// slow isolate.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PollingObservers::DeliveryState {
  Snapshot delivered;
  std::optional<PendingChange> pending;
  bool awaiting_reply = false;
};

// Purpose: Hold one observer's settings, worker state and change state.
// @param spec is fixed when the observer starts.
// @param stop_requested is given to each snapshot, so a long scan ends once
// the observer stops.
// @param mutex guards delivery, and orders a stop request with the worker's
// wait.
// @param lifecycle is what the worker needs once it may outlive its owner.
// @param delivery is guarded by mutex.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PollingObservers::Observer {
  // Purpose: Hold the settings for one observer before its worker starts.
  // @param observer_spec describes what to poll and how often.
  // @returns Nothing.
  // @throws Nothing.
  explicit Observer(ObserverSpec observer_spec)
      : spec(std::move(observer_spec)),
        stop_requested([this]() { return lifecycle.stopping.load(); }) {}

  const ObserverSpec spec;
  const StopRequested stop_requested;
  std::mutex mutex;
  WorkerLifecycle lifecycle;
  DeliveryState delivery;
};

// Purpose: Prepare to deliver events through effects.send_event.
// @param effects supplies event delivery and the clock.
// @returns Nothing; check IsReady() before starting observers.
// @throws Nothing; a failed setup leaves IsReady() false.
PollingObservers::PollingObservers(PollingObserverEffects effects)
    : effects_(std::move(effects)),
      dispatcher_(std::make_shared<PlatformThreadDispatcher>(
          [this]() { DeliverPendingChanges(); })) {}

// Purpose: Stop every observer without waiting for its worker, then close
// delivery. Changes not yet delivered are dropped.
// @returns Nothing.
// @throws Nothing.
PollingObservers::~PollingObservers() {
  for (auto& entry : observers_) {
    RequestStop(*entry.second);
  }
  observers_.clear();
  // Closed on the platform thread, so a worker still finishing a snapshot
  // can no longer wake this destroyed object.
  dispatcher_->Close();
}

// Purpose: Report whether events can reach the platform thread.
// @returns true when observers can be started.
// @throws Nothing.
bool PollingObservers::IsReady() const { return dispatcher_->IsReady(); }

// Purpose: Start polling one domain on a new worker thread.
// @param spec is fixed when the observer starts.
// @param baseline is the snapshot taken before the start was acknowledged.
// @returns The new observer's id, or nothing when too many worker threads
// are still running.
// @throws std::system_error when the worker thread cannot be started.
std::optional<std::string> PollingObservers::Start(ObserverSpec spec, Snapshot baseline) {
  auto observer = std::make_shared<Observer>(std::move(spec));
  observer->lifecycle.dispatcher = dispatcher_;
  observer->lifecycle.now_epoch_ms = effects_.now_epoch_ms;
  observer->delivery.delivered = std::move(baseline);
  // Started before the observer is stored, so a failed start leaves nothing
  // behind; no event can reach DeliverPendingChanges until it is stored,
  // because wakes run on this same thread.
  if (StartDetachedThread([observer]() { RunWorker(observer); }) == DetachedStart::kAtLimit) {
    return std::nullopt;
  }
  const std::string observer_id = "windows_observer_" + std::to_string(++observer_counter_);
  observers_[observer_id] = std::move(observer);
  return observer_id;
}

// Purpose: Stop one observer without waiting for its worker. Its
// undelivered change is dropped. An unknown id does nothing.
// @param observer_id is the id Start returned.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::Stop(const std::string& observer_id) {
  auto it = observers_.find(observer_id);
  if (it == observers_.end()) {
    return;
  }
  RequestStop(*it->second);
  observers_.erase(it);
}

// Purpose: Poll one observer until it is stopped.
// @param observer is the observer this worker serves.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::RunWorker(const std::shared_ptr<Observer>& observer) {
  FailureStreaks streaks;
  while (!WaitForNextPoll(*observer)) {
    std::optional<Snapshot> current = TakeSnapshot(*observer, &streaks);
    // A failed poll still retries the wake for a change found earlier, so a
    // wake the queue rejected is not stranded behind a run of failed reads.
    const bool change_pending = current.has_value()
                                    ? RecordSnapshot(*observer, std::move(*current))
                                    : HasPendingChange(*observer);
    if (change_pending) {
      RequestDelivery(*observer, &streaks);
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
std::optional<Snapshot> PollingObservers::TakeSnapshot(Observer& observer,
                                                       FailureStreaks* streaks) {
  std::optional<Snapshot> snapshot;
  std::string failure;
  try {
    snapshot = observer.spec.take_snapshot(observer.stop_requested);
  } catch (const std::exception& error) {
    // An exception leaving a worker thread ends the app, so the worker is
    // the nearest place that can handle a failed read: the poll is skipped
    // and the next one tries again.
    failure = ExceptionText(error);
  }
  if (observer.lifecycle.stopping.load()) {
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
              {{"domain", observer.spec.domain},
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
bool PollingObservers::WaitForNextPoll(Observer& observer) {
  std::unique_lock<std::mutex> lock(observer.mutex);
  return observer.lifecycle.stop_signal.wait_for(
      lock, observer.spec.polling_interval,
      [&observer]() { return observer.lifecycle.stopping.load(); });
}

// Purpose: Ask the platform thread to deliver pending changes, logging a
// failure once per streak.
// @param observer is the observer with a pending change.
// @param streaks records which failures this worker has logged.
// @returns Nothing; a pending change whose wake failed is retried after
// the next poll.
// @throws Nothing.
void PollingObservers::RequestDelivery(Observer& observer, FailureStreaks* streaks) {
  if (observer.lifecycle.dispatcher->RequestWake()) {
    streaks->wake_failing = false;
    return;
  }
  // Read before logging, which can change the thread's last error.
  const DWORD error = GetLastError();
  // A stopped observer's wake fails because its owner closed delivery;
  // that is teardown, not a fault.
  if (streaks->wake_failing || observer.lifecycle.stopping.load()) {
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
bool PollingObservers::RecordSnapshot(Observer& observer, Snapshot current) {
  std::lock_guard<std::mutex> lock(observer.mutex);
  DeliveryState& delivery = observer.delivery;
  if (current == delivery.delivered) {
    // Back to what was last delivered: nothing is left to report.
    delivery.pending.reset();
    return false;
  }
  const bool already_pending =
      delivery.pending.has_value() && delivery.pending->snapshot == current;
  if (!already_pending) {
    delivery.pending = PendingChange{ChangedIds(delivery.delivered, current),
                                     std::move(current),
                                     observer.lifecycle.now_epoch_ms()};
  }
  return true;
}

// Purpose: Report whether an observer holds a change not yet delivered.
// @param observer is the observer to check.
// @returns true when a change is pending.
// @throws Nothing.
bool PollingObservers::HasPendingChange(Observer& observer) {
  std::lock_guard<std::mutex> lock(observer.mutex);
  return observer.delivery.pending.has_value();
}

// Purpose: Deliver every pending change of an observer with no event
// awaiting Dart's reply. Runs on the platform thread.
// @returns Nothing.
// @throws Nothing beyond what effects.send_event throws.
void PollingObservers::DeliverPendingChanges() {
  std::vector<ObserveEvent> events;
  for (auto& [observer_id, observer] : observers_) {
    std::lock_guard<std::mutex> observer_lock(observer->mutex);
    DeliveryState& delivery = observer->delivery;
    if (!delivery.pending.has_value() || delivery.awaiting_reply) {
      continue;
    }
    events.push_back({.observer_id = observer_id,
                      .event = BuildEvent(observer->spec.domain, *delivery.pending),
                      .on_settled = [sender = std::weak_ptr<Observer>(observer)]() {
                        SettleEvent(sender);
                      }});
    delivery.delivered = std::move(delivery.pending->snapshot);
    delivery.pending.reset();
    delivery.awaiting_reply = true;
  }
  // Sent outside the locks. Stop runs on this same thread, so no observer
  // can stop between collecting its event and sending it.
  for (ObserveEvent& event : events) {
    effects_.send_event(std::move(event));
  }
}

// Purpose: Mark an observer's event as answered, and ask for delivery of a
// change that arrived meanwhile. Runs on the platform thread.
// @param observer is the observer, or empty once it is gone.
// @returns Nothing.
// @throws Nothing.
void PollingObservers::SettleEvent(const std::weak_ptr<Observer>& observer) {
  const std::shared_ptr<Observer> live = observer.lock();
  if (!live) {
    return;
  }
  bool change_waiting = false;
  {
    std::lock_guard<std::mutex> lock(live->mutex);
    live->delivery.awaiting_reply = false;
    change_waiting = live->delivery.pending.has_value();
  }
  // A failed wake is retried by the worker, which asks again after each poll
  // while the change is still pending.
  if (change_waiting && !live->lifecycle.stopping.load()) {
    live->lifecycle.dispatcher->RequestWake();
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
  // Dart parses the timestamp with DateTime.parse, so it is ISO 8601.
  event[EncodableValue("timestamp")] = EncodableValue(FormatIsoUtc(
      std::chrono::system_clock::time_point(
          std::chrono::milliseconds(change.detected_at_epoch_ms))));
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
