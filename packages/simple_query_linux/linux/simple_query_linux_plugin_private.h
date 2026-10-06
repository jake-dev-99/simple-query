#ifndef SIMPLE_QUERY_LINUX_PLUGIN_PRIVATE_H_
#define SIMPLE_QUERY_LINUX_PLUGIN_PRIVATE_H_

#include <flutter_linux/flutter_linux.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "native_query.g.h"
#ifdef HAS_LIBEBOOK
#include <libebook/libebook.h>
#endif
#ifdef HAS_LIBECAL
#include <libecal/libecal.h>
#endif

namespace simple_query_linux {

/** Purpose: Make unique Flutter values release their native reference safely.
 * @param value is supplied to the deleter when an owned value leaves scope.
 * @returns A stateless deletion policy for ValuePtr.
 * @throws Nothing. */
struct FlValueDeleter {
  /** Purpose: Release a uniquely owned Flutter value through its C API.
   * @param value is the nullable Flutter value leaving scope.
   * @returns Nothing. @throws Nothing. */
  void operator()(FlValue* value) const;
};

using ValuePtr = std::unique_ptr<FlValue, FlValueDeleter>;
using Rows = std::vector<ValuePtr>;
using Snapshot = std::map<std::string, std::string>;

/** Purpose: Keep snapshot failures distinct from a successfully empty source.
 * @param snapshot maps record IDs to complete change signatures.
 * @param error preserves the diagnostic when snapshot acquisition fails.
 * @returns An owned snapshot result for startup validation and polling.
 * @throws std::bad_alloc when owned snapshot or error fields are populated. */
struct SnapshotResult {
  Snapshot snapshot;
  std::optional<std::string> error;
};

/** Purpose: Preserve a stable error category separately from its diagnostic.
 * @param code is the portable native error identifier.
 * @param message describes the failure with the simple_query prefix.
 * @returns An owned error for generated response envelopes.
 * @throws std::bad_alloc when error strings are allocated. */
struct NativeError {
  std::string code;
  std::string message;
};

/** Purpose: Transfer payload ownership without hiding native operation errors.
 * @param value owns the successful Flutter payload, if available.
 * @param error carries the failure instead of a successful payload.
 * @returns A move-only operation result for callback translation.
 * @throws std::bad_alloc when an error diagnostic is allocated. */
struct ValueResult {
  ValuePtr value;
  std::optional<NativeError> error;
};

/** Purpose: Keep native identifiers separate from failed operation results.
 * @param value owns the successful identifier, if available.
 * @param error carries the failure instead of an identifier.
 * @returns An owned string result for callback translation.
 * @throws std::bad_alloc when identifier or error strings are allocated. */
struct StringResult {
  std::string value;
  std::optional<NativeError> error;
};

struct ObserverState;

ValuePtr Value(FlValue* value);
void MapSet(FlValue* map, const char* key, FlValue* value);
void MapSetString(FlValue* map, const char* key, const std::string& value);
void MapSetBool(FlValue* map, const char* key, bool value);
void MapSetInt(FlValue* map, const char* key, int64_t value);
FlValue* FindValue(FlValue* map, const std::string& key);
std::optional<std::string> AsString(FlValue* value);
std::string StringOr(FlValue* map, const std::string& key,
                     const std::string& fallback);
std::optional<int64_t> AsInt(FlValue* value);
bool BoolOr(FlValue* map, const std::string& key, bool fallback);
FlValue* AsMap(FlValue* value);
FlValue* AsList(FlValue* value);
ValuePtr CloneMap(FlValue* map);
std::string Lower(std::string value);
std::string MimeFromPath(const std::filesystem::path& path);
std::string FileSystemFailure(const char* operation,
                              const std::filesystem::path& path,
                              const std::error_code& error);
std::string ValueAsString(FlValue* row, const std::string& key);
ValuePtr Capability(const std::string& domain, bool can_read, bool can_write,
                    bool can_observe, bool can_stream,
                    const std::optional<std::string>& reason = std::nullopt);
std::filesystem::path ResolveRootPath(FlValue* request);
Rows ApplyFilters(Rows rows, FlValue* filters);
void ApplySort(Rows* rows, FlValue* sort);
ValuePtr ApplyProjection(const Rows& rows, FlValue* projection);
ValueResult Success(ValuePtr value);
ValueResult Failure(const std::string& code, const std::string& message);

/** Purpose: Build one complete observer snapshot without masking failures.
 * @param request is the independently owned native request.
 * @param domain selects files, media, contacts, or calendar.
 * @param cancellable optionally interrupts native acquisition.
 * @returns A record signature map or stable failure.
 * @throws Nothing. */
SnapshotResult BuildSnapshotForDomain(FlValue* request,
                                      const std::string& domain,
                                      GCancellable* cancellable);

/** Purpose: Encode all projected row fields into one exact change signature.
 * @param row is the borrowed portable record.
 * @returns Codec bytes or a stable encoding failure.
 * @throws std::bad_alloc when copying encoded bytes fails. */
StringResult SnapshotSignature(FlValue* row);
#ifdef HAS_LIBEBOOK
/** Purpose: Project a real EDS contact to the portable Linux row schema.
 * @param contact is the borrowed native contact.
 * @returns A newly owned portable row.
 * @throws Nothing. */
ValuePtr ProjectContactRecord(EContact* contact);
#endif
#ifdef HAS_LIBECAL
/** Purpose: Project a real EDS event to the portable Linux row schema.
 * @param component is the borrowed native event.
 * @param calendar_uid identifies its source calendar.
 * @returns A newly owned portable row.
 * @throws Nothing. */
ValuePtr ProjectCalendarRecord(ICalComponent* component,
                               const gchar* calendar_uid);
#endif

/** Purpose: Coordinate Linux query, mutation, binary, and observer domains.
 * @param messenger initializes transport on the registrar's platform context.
 * @returns A uniquely owned host with shared, independently cancellable workers.
 * @throws Native allocation errors for callback boundaries to translate.
 * Ownership: The plugin uniquely owns this host; observer state is shared only
 * while workers or queued deliveries require it.
 */
class NativeQueryHostApiImpl {
 public:
  /** Purpose: Bind Linux behavior to the registrar-owned Flutter messenger.
   * @param messenger is the borrowed platform binary messenger.
   * @returns A host retaining its Flutter API and platform context.
   * @throws Nothing. */
  explicit NativeQueryHostApiImpl(FlBinaryMessenger* messenger);

  /** Purpose: Stop observers and release platform resources.
   * @param None.
   * @returns Nothing.
   * @throws Nothing. */
  ~NativeQueryHostApiImpl();

  ValueResult GetCapabilities();
  ValueResult Query(FlValue* request);
  ValueResult Mutate(FlValue* request);
  ValueResult Batch(FlValue* request);
  StringResult ObserveStart(FlValue* request);
  std::optional<NativeError> ObserveStop(const std::string& observer_id);
  ValueResult OpenBinary(FlValue* request);
  std::optional<NativeError> CloseBinary(const std::string& handle_id);
  ValueResult CallExtension(const std::string& name_space,
                            const std::string& method, FlValue* args);

 private:
  struct ObserveDelivery;
  struct ObserveCompletion;
  struct ObserverWork;

  /** Purpose: Release a queued observer delivery.
   * @param user_data owns the queued delivery.
   * @returns Nothing.
   * @throws Nothing. */
  static void DestroyObserveDelivery(gpointer user_data);
  /** Purpose: Finish a generated FlutterApi delivery.
   * @param object is the generated API source.
   * @param result is the asynchronous transport result.
   * @param user_data owns completion state.
   * @returns Nothing.
   * @throws Nothing. */
  static void FinishObserveDelivery(GObject* object, GAsyncResult* result,
                                    gpointer user_data);
  /** Purpose: Begin one queued delivery on the platform context.
   * @param user_data owns the queued event.
   * @returns G_SOURCE_REMOVE after dispatch or cancellation.
   * @throws Nothing. */
  static gboolean DeliverObserveEvent(gpointer user_data);
  /** Purpose: Queue one bounded platform-context delivery.
   * @param observer_id identifies the Dart stream.
   * @param state owns transport and cancellation state.
   * @param event is transferred into the queue.
   * @returns True when queued.
   * @throws std::bad_alloc when queue allocation fails. */
  static bool QueueObserveEvent(
      const std::string& observer_id,
      const std::shared_ptr<ObserverState>& state, ValuePtr event);
  /** Purpose: Poll a domain on one detached GLib worker.
   * @param observer_id identifies the Dart stream.
   * @param domain selects native acquisition.
   * @param request is exclusively worker-owned.
   * @param interval_ms is the minimum polling cadence.
   * @param previous is the validated initial snapshot.
   * @param state owns cancellation and transport lifetime.
   * @returns Nothing.
   * @throws Nothing. */
  static void RunObserver(
      const std::string& observer_id, const std::string& domain,
      ValuePtr request, int64_t interval_ms, Snapshot previous,
      const std::shared_ptr<ObserverState>& state) noexcept;
  /** Purpose: Transfer owned polling work into GThread execution.
   * @param user_data owns an ObserverWork instance.
   * @returns Null after polling ends.
   * @throws Nothing. */
  static gpointer RunObserverThread(gpointer user_data);
  /** Purpose: Cancel worker and queued delivery activity without joining.
   * @param state is removed from the public registry.
   * @returns Nothing.
   * @throws Nothing. */
  void StopObserver(const std::shared_ptr<ObserverState>& state);
  /** Purpose: Stop every registered observer during plugin disposal.
   * @param None.
   * @returns Nothing.
   * @throws Nothing. */
  void ShutdownObservers();

  SqlqNativeQueryFlutterApi* flutter_api_;
  GMainContext* platform_context_;
  int64_t handle_counter_ = 0;
  int64_t observer_counter_ = 0;
  std::map<std::string, std::string> open_handles_;
  std::map<std::string, std::shared_ptr<ObserverState>> observers_;
  std::mutex observers_mutex_;
};

}  // namespace simple_query_linux

#endif  // SIMPLE_QUERY_LINUX_PLUGIN_PRIVATE_H_
