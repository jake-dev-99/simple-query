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

namespace simple_query_linux {

struct FlValueDeleter {
  /** Purpose: Release a uniquely owned Flutter value through its C API.
   * @param value is the nullable Flutter value leaving scope.
   * @returns Nothing. @throws Nothing. */
  void operator()(FlValue* value) const;
};

using ValuePtr = std::unique_ptr<FlValue, FlValueDeleter>;
using Rows = std::vector<ValuePtr>;
using Snapshot = std::map<std::string, int64_t>;

/** Purpose: Carry a filesystem snapshot or the source failure that blocked it.
 */
struct SnapshotResult {
  Snapshot snapshot;
  std::optional<std::string> error;
};

/** Purpose: Preserve one stable native error code and diagnostic message. */
struct NativeError {
  std::string code;
  std::string message;
};

/** Purpose: Return either an owned Flutter payload or one native error. */
struct ValueResult {
  ValuePtr value;
  std::optional<NativeError> error;
};

/** Purpose: Return either an owned string payload or one native error. */
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
SnapshotResult BuildSnapshotForDomain(FlValue* request,
                                      const std::string& domain);

/** Purpose: Coordinate Linux query, mutation, binary, and observer domains.
 * Ownership: The plugin uniquely owns this host; observer state is shared only
 * while workers or queued deliveries require it.
 */
class NativeQueryHostApiImpl {
 public:
  /** Purpose: Bind Linux behavior to the registrar-owned Flutter messenger.
   * @param messenger is the borrowed platform binary messenger.
   * @throws Nothing. */
  explicit NativeQueryHostApiImpl(FlBinaryMessenger* messenger);

  /** Purpose: Stop observers and release platform resources.
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

  static void DestroyObserveDelivery(gpointer user_data);
  static void FinishObserveDelivery(GObject* object, GAsyncResult* result,
                                    gpointer user_data);
  static gboolean DeliverObserveEvent(gpointer user_data);
  bool QueueObserveEvent(const std::string& observer_id,
                         const std::shared_ptr<ObserverState>& state,
                         ValuePtr event);
  void RunObserver(const std::string& observer_id, const std::string& domain,
                   ValuePtr request, int64_t interval_ms,
                   const std::shared_ptr<ObserverState>& state) noexcept;
  void StopObserver(const std::shared_ptr<ObserverState>& state);
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
