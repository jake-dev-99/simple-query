#ifndef SIMPLE_QUERY_WINDOWS_POLLING_OBSERVERS_H_
#define SIMPLE_QUERY_WINDOWS_POLLING_OBSERVERS_H_

#include <flutter/encodable_value.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "platform_thread_dispatcher.h"

namespace simple_query_windows {

// Each watched record's id, mapped to the value that changes when the record
// changes, such as its modification time.
using Snapshot = std::map<std::string, int64_t>;

// Returns true once the observer that asked for a snapshot is stopping, so a
// long scan can end early. A partial snapshot taken then is discarded.
using StopRequested = std::function<bool()>;

// Purpose: Describe one polled observation.
struct ObserverSpec {
  // The query domain reported in each event, such as "files".
  std::string domain;
  // How long the worker waits between snapshots.
  std::chrono::milliseconds polling_interval;
  // Lists the watched records. Runs on the observer's worker thread.
  std::function<Snapshot(const StopRequested&)> take_snapshot;
};

// Purpose: Give PollingObservers its effects, so tests can supply their own.
struct PollingObserverEffects {
  // Sends one event. Runs only on the thread that created PollingObservers.
  std::function<void(const std::string& observer_id,
                     const flutter::EncodableMap& event)>
      send_event;
  // Returns the current time in milliseconds since the Unix epoch.
  std::function<int64_t()> now_epoch_ms;
};

// Purpose: Poll each observed domain on its own worker thread and deliver
// changes on the thread that created this object, the platform thread.
//
// Memory stays bounded however long the platform thread is busy. Each
// observer keeps the snapshot it last delivered and at most one pending
// change. A pending change holds the latest snapshot and the ids that differ
// from the delivered one, so later polls replace it instead of queueing more.
// Its ids still cover every change since the last delivery, because they are
// measured against the delivered snapshot. A change that is undone before
// delivery is not reported.
//
// Create, call and destroy this object on the platform thread.
class PollingObservers {
 public:
  // Purpose: Prepare to deliver events through effects.send_event.
  // @param effects supplies event delivery and the clock.
  // @returns Nothing; check IsReady() before starting observers.
  // @throws Nothing; a failed setup leaves IsReady() false.
  explicit PollingObservers(PollingObserverEffects effects);

  // Purpose: Stop every observer, then release delivery resources.
  // Changes not yet delivered are dropped.
  // @returns Nothing.
  // @throws Nothing.
  ~PollingObservers();

  PollingObservers(const PollingObservers&) = delete;
  PollingObservers& operator=(const PollingObservers&) = delete;

  // Purpose: Report whether events can reach the platform thread.
  // @returns true when observers can be started.
  // @throws Nothing.
  bool IsReady() const;

  // Purpose: Start polling one domain on a new worker thread.
  // @param spec describes what to poll and how often.
  // @returns The new observer's id, used in its events and to stop it.
  // @throws std::system_error when the worker thread cannot be started.
  std::string Start(ObserverSpec spec);

  // Purpose: Stop one observer and wait for its worker to finish. Its
  // undelivered change is dropped. An unknown id does nothing.
  // @param observer_id is the id Start returned.
  // @returns Nothing.
  // @throws Nothing.
  void Stop(const std::string& observer_id);

 private:
  // Purpose: Hold one observer's settings, worker and change state.
  struct Observer;

  // Purpose: A change detected but not yet delivered.
  struct PendingChange {
    flutter::EncodableList ids;
    Snapshot snapshot;
    int64_t detected_at_epoch_ms = 0;
  };

  // Purpose: Poll one observer until it is stopped.
  // @param observer is the observer this worker serves; it outlives the
  // worker, which is always joined before the observer is destroyed.
  // @returns Nothing.
  // @throws Nothing.
  void RunWorker(Observer* observer);

  // Purpose: Store a new snapshot as the observer's pending change.
  // @param observer is the observer that took the snapshot.
  // @param current is the snapshot just taken.
  // @returns true when a change is pending and needs delivery.
  // @throws Nothing.
  bool RecordSnapshot(Observer* observer, Snapshot current);

  // Purpose: Deliver every pending change. Runs on the platform thread.
  // @returns Nothing.
  // @throws Nothing beyond what effects.send_event throws.
  void DeliverPendingChanges();

  // Purpose: Build the event map sent for one change.
  // @param domain is the observed domain.
  // @param change is the change being delivered.
  // @returns The event map.
  // @throws Nothing.
  static flutter::EncodableMap BuildEvent(const std::string& domain,
                                          const PendingChange& change);

  // Purpose: Wake an observer's worker and tell it to finish.
  // @param observer is the observer to stop.
  // @returns Nothing.
  // @throws Nothing.
  static void RequestStop(Observer& observer);

  // Purpose: Wait for an observer's worker to finish.
  // @param observer is an observer whose stop was already requested.
  // @returns Nothing.
  // @throws Nothing.
  static void JoinWorker(Observer& observer);

  PollingObserverEffects effects_;
  std::mutex observers_mutex_;
  int64_t observer_counter_ = 0;
  std::map<std::string, std::unique_ptr<Observer>> observers_;
  // Declared last so it is destroyed first; the destructor has already
  // joined every worker by then.
  PlatformThreadDispatcher dispatcher_;
};

// Purpose: List the ids whose values differ between two snapshots, including
// ids present in only one of them.
// @param before is the earlier snapshot.
// @param after is the later snapshot.
// @returns The differing ids, in ascending order.
// @throws Nothing.
flutter::EncodableList ChangedIds(const Snapshot& before, const Snapshot& after);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_POLLING_OBSERVERS_H_
