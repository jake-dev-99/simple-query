#include "polling_observers.h"

#include <iostream>
#include <set>
#include <utility>
#include <vector>

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
      : spec(std::move(observer_spec)) {}

  const ObserverSpec spec;
  // Read without the mutex by snapshot scans; written under it.
  std::atomic<bool> stopping{false};
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
  // The worker never reads the observer map, so it may start first.
  observer->worker =
      std::thread([this, worker_observer]() { RunWorker(worker_observer); });

  std::lock_guard<std::mutex> lock(observers_mutex_);
  std::string observer_id =
      "windows_observer_" + std::to_string(++observer_counter_);
  observers_[observer_id] = std::move(observer);
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
  const StopRequested stop_requested = [observer]() {
    return observer->stopping.load();
  };
  Snapshot initial = observer->spec.take_snapshot(stop_requested);
  {
    std::lock_guard<std::mutex> lock(observer->mutex);
    observer->delivered = std::move(initial);
  }

  while (true) {
    {
      std::unique_lock<std::mutex> lock(observer->mutex);
      const bool stop = observer->stop_signal.wait_for(
          lock, observer->spec.polling_interval,
          [observer]() { return observer->stopping.load(); });
      if (stop) {
        return;
      }
    }
    Snapshot current = observer->spec.take_snapshot(stop_requested);
    if (stop_requested()) {
      return;
    }
    if (RecordSnapshot(observer, std::move(current)) &&
        !dispatcher_.RequestWake()) {
      // The pending change stays recorded, so the next poll asks again.
      std::cerr << "simple_query_windows: could not schedule an observe event "
                   "on the platform thread (Windows error "
                << GetLastError() << "); retrying after the next poll.\n";
    }
  }
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

}  // namespace simple_query_windows
