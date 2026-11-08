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
#include <vector>

#include "platform_thread_dispatcher.h"
#include "stop_requested.h"

namespace simple_query_windows {

// Each watched record's id, mapped to a fingerprint of its row that changes
// whenever any field of the record changes.
using Snapshot = std::map<std::string, int64_t>;

// Purpose: Describe one polled observation.
struct ObserverSpec {
  // The query domain reported in each event, such as "files".
  std::string domain;
  // How long the worker waits between snapshots.
  std::chrono::milliseconds polling_interval;
  // Lists the watched records. Runs on the observer's worker thread. Throws
  // a std::exception when the records could not be read; that poll is then
  // skipped. Its stop check turns true once the observer is stopping, so a
  // long scan can end early; a snapshot taken then is discarded.
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
// A poll whose snapshot fails changes nothing, so a passing read error never
// reports records as removed and then added again. Until the first snapshot
// succeeds, the observer has no baseline and reports nothing.
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

  // Purpose: Hold one observer's worker thread and how it is told to stop.
  struct WorkerLifecycle;

  // Purpose: Hold what one observer last delivered and what waits to go.
  struct DeliveryState;

  // Purpose: Remember which failures a worker has logged. A failure that
  // repeats on every poll is logged once, then again only after it clears.
  struct FailureStreaks {
    bool snapshot_failing = false;
    bool wake_failing = false;
  };

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

  // Purpose: Take one snapshot for a worker, logging a failure once per
  // streak.
  // @param observer is the observer being polled.
  // @param streaks records which failures this worker has logged.
  // @returns The snapshot, or nothing when it failed or the observer is
  // stopping.
  // @throws Nothing.
  static std::optional<Snapshot> TakeSnapshot(Observer* observer,
                                              FailureStreaks* streaks);

  // Purpose: Wait one polling interval, or less when the observer stops.
  // @param observer is the observer being polled.
  // @returns true when the observer is stopping.
  // @throws Nothing.
  static bool WaitForNextPoll(Observer* observer);

  // Purpose: Ask the platform thread to deliver pending changes, logging a
  // failure once per streak.
  // @param streaks records which failures this worker has logged.
  // @returns Nothing; a pending change whose wake failed is retried after
  // the next poll.
  // @throws Nothing.
  void RequestDelivery(FailureStreaks* streaks);

  // Purpose: Store a new snapshot as the observer's pending change.
  // @param observer is the observer that took the snapshot.
  // @param current is the snapshot just taken.
  // @returns true when a change is pending and needs delivery.
  // @throws Nothing.
  bool RecordSnapshot(Observer* observer, Snapshot current);

  // Purpose: Report whether an observer holds a change not yet delivered.
  // @param observer is the observer to check.
  // @returns true when a change is pending.
  // @throws Nothing.
  bool HasPendingChange(Observer* observer);

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

// Purpose: Build a snapshot that changes whenever any field of a row changes.
// @param rows are the records, each with a string "id" field.
// @returns Each id mapped to a hash of its whole row. Rows without a string id
// are left out.
// @throws std::invalid_argument when a row holds a custom value, which has no
// bytes to hash; std::logic_error for a value type the hash does not know.
Snapshot FingerprintRecords(const std::vector<flutter::EncodableMap>& rows);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_POLLING_OBSERVERS_H_
