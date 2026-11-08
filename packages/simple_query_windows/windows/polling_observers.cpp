#include "polling_observers.h"

#include <flutter/standard_message_codec.h>

#include <exception>
#include <iostream>
#include <set>
#include <string_view>
#include <utility>

namespace simple_query_windows {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

struct PollingObservers::Observer {
  // Purpose: Hold the settings for one observer before its worker starts.
  // @param observer_spec describes what to poll and how often.
  // @returns Nothing.
  // @throws Nothing.
  explicit Observer(ObserverSpec observer_spec)
      : spec(std::move(observer_spec)),
        stop_requested([this]() { return stopping.load(); }) {}

  const ObserverSpec spec;
  // Read without the mutex by snapshot scans; written under it.
  std::atomic<bool> stopping{false};
  // Given to each snapshot so a long scan ends once the observer stops.
  const StopRequested stop_requested;
  std::mutex mutex;
  // Wakes the worker early when the observer is stopping.
  std::condition_variable stop_signal;
  // Guarded by mutex: the snapshot the last delivered event described.
  Snapshot delivered;
  // Guarded by mutex: the change waiting for delivery, if any.
  std::optional<PendingChange> pending;
  std::thread worker;
};

PollingObservers::PollingObservers(PollingObserverEffects effects)
    : effects_(std::move(effects)),
      dispatcher_([this]() { DeliverPendingChanges(); }) {}

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

bool PollingObservers::IsReady() const { return dispatcher_.IsReady(); }

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
    worker_observer->worker =
        std::thread([this, worker_observer]() { RunWorker(worker_observer); });
  } catch (...) {
    // No worker started, so the observer is removed and the caller told.
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.erase(observer_id);
    throw;
  }
  return observer_id;
}

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
    observer->delivered = std::move(*baseline);
  }

  while (!WaitForNextPoll(observer)) {
    std::optional<Snapshot> current = TakeSnapshot(observer, &streaks);
    if (current.has_value() && RecordSnapshot(observer, std::move(*current))) {
      RequestDelivery(&streaks);
    }
  }
}

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
  if (observer->stopping.load()) {
    return std::nullopt;
  }
  if (snapshot.has_value()) {
    streaks->snapshot_failing = false;
    return snapshot;
  }
  if (!streaks->snapshot_failing) {
    streaks->snapshot_failing = true;
    std::cerr << "simple_query_windows: an observe snapshot of "
              << observer->spec.domain << " failed (" << failure
              << "); retrying each poll, and not logging again until one "
                 "succeeds.\n";
  }
  return std::nullopt;
}

bool PollingObservers::WaitForNextPoll(Observer* observer) {
  std::unique_lock<std::mutex> lock(observer->mutex);
  return observer->stop_signal.wait_for(
      lock, observer->spec.polling_interval,
      [observer]() { return observer->stopping.load(); });
}

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
  std::cerr << "simple_query_windows: could not schedule an observe event "
               "on the platform thread (Windows error "
            << error << "); retrying after each poll, and not logging again "
                        "until one is scheduled.\n";
}

bool PollingObservers::RecordSnapshot(Observer* observer, Snapshot current) {
  std::lock_guard<std::mutex> lock(observer->mutex);
  if (current == observer->delivered) {
    // Back to what was last delivered: nothing is left to report.
    observer->pending.reset();
    return false;
  }
  const bool already_pending = observer->pending.has_value() &&
                               observer->pending->snapshot == current;
  if (!already_pending) {
    observer->pending = PendingChange{ChangedIds(observer->delivered, current),
                                      std::move(current),
                                      effects_.now_epoch_ms()};
  }
  return true;
}

void PollingObservers::DeliverPendingChanges() {
  std::vector<std::pair<std::string, EncodableMap>> events;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    for (auto& [observer_id, observer] : observers_) {
      std::lock_guard<std::mutex> observer_lock(observer->mutex);
      if (!observer->pending.has_value()) {
        continue;
      }
      events.emplace_back(observer_id,
                          BuildEvent(observer->spec.domain, *observer->pending));
      observer->delivered = std::move(observer->pending->snapshot);
      observer->pending.reset();
    }
  }
  // Send outside the locks; Stop runs on this same thread, so no observer
  // can stop between collecting its event and sending it.
  for (const auto& [observer_id, event] : events) {
    effects_.send_event(observer_id, event);
  }
}

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

void PollingObservers::RequestStop(Observer& observer) {
  {
    // Set under the mutex so a worker between its stop check and its wait
    // cannot miss the signal.
    std::lock_guard<std::mutex> lock(observer.mutex);
    observer.stopping.store(true);
  }
  observer.stop_signal.notify_all();
}

void PollingObservers::JoinWorker(Observer& observer) {
  if (observer.worker.joinable()) {
    observer.worker.join();
  }
}

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

Snapshot FingerprintRecords(const std::vector<EncodableMap>& rows) {
  const auto& codec = flutter::StandardMessageCodec::GetInstance();
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
    // The map's keys are ordered, so equal rows always encode to equal bytes.
    const auto bytes = codec.EncodeMessage(EncodableValue(row));
    const std::string_view encoded(reinterpret_cast<const char*>(bytes->data()),
                                   bytes->size());
    snapshot[*id_text] =
        static_cast<int64_t>(std::hash<std::string_view>{}(encoded));
  }
  return snapshot;
}

}  // namespace simple_query_windows
