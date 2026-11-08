#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <flutter/plugin_registrar_windows.h>
#include <winrt/base.h>

#include "file_records.h"
#include "native_query.g.h"
#include "polling_observers.h"
#include "structured_log.h"
#include "winrt_records.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

const EncodableValue* FindValue(const EncodableMap& map, const std::string& key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &it->second;
}

std::optional<std::string> AsString(const EncodableValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* str = std::get_if<std::string>(value)) {
    return *str;
  }
  if (const auto* i32 = std::get_if<int32_t>(value)) {
    return std::to_string(*i32);
  }
  if (const auto* i64 = std::get_if<int64_t>(value)) {
    return std::to_string(*i64);
  }
  if (const auto* dbl = std::get_if<double>(value)) {
    return std::to_string(*dbl);
  }
  if (const auto* b = std::get_if<bool>(value)) {
    return *b ? "true" : "false";
  }
  return std::nullopt;
}

std::string StringOr(const EncodableMap& map, const std::string& key,
                     const std::string& fallback) {
  const auto* value = FindValue(map, key);
  const auto parsed = AsString(value);
  return parsed.value_or(fallback);
}

std::optional<int64_t> AsInt(const EncodableValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* i32 = std::get_if<int32_t>(value)) {
    return static_cast<int64_t>(*i32);
  }
  if (const auto* i64 = std::get_if<int64_t>(value)) {
    return *i64;
  }
  if (const auto* dbl = std::get_if<double>(value)) {
    return static_cast<int64_t>(*dbl);
  }
  return std::nullopt;
}

bool BoolOr(const EncodableMap& map, const std::string& key, bool fallback) {
  const auto* value = FindValue(map, key);
  if (value == nullptr) {
    return fallback;
  }
  if (const auto* b = std::get_if<bool>(value)) {
    return *b;
  }
  return fallback;
}

const EncodableMap* AsMap(const EncodableValue* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return std::get_if<EncodableMap>(value);
}

const EncodableList* AsList(const EncodableValue* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return std::get_if<EncodableList>(value);
}

int64_t EpochMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string ValueAsString(const EncodableMap& row, const std::string& key) {
  auto it = row.find(EncodableValue(key));
  if (it == row.end()) {
    return "";
  }
  return AsString(&it->second).value_or("");
}

EncodableMap Capability(const std::string& domain, bool can_read, bool can_write,
                        bool can_observe, bool can_stream,
                        const std::optional<std::string>& reason = std::nullopt) {
  EncodableMap map;
  map[EncodableValue("domain")] = EncodableValue(domain);
  map[EncodableValue("canRead")] = EncodableValue(can_read);
  map[EncodableValue("canWrite")] = EncodableValue(can_write);
  map[EncodableValue("canObserve")] = EncodableValue(can_observe);
  map[EncodableValue("canStream")] = EncodableValue(can_stream);
  if (reason.has_value()) {
    map[EncodableValue("reason")] = EncodableValue(*reason);
  }
  return map;
}

std::filesystem::path ResolveRootPath(const EncodableMap& request) {
  const EncodableMap* platform_data = AsMap(FindValue(request, "platformData"));
  if (platform_data != nullptr) {
    const auto root = AsString(FindValue(*platform_data, "rootPath"));
    if (root.has_value() && !root->empty()) {
      return std::filesystem::path(*root);
    }
  }
  return std::filesystem::current_path();
}

std::vector<EncodableMap> ApplyFilters(std::vector<EncodableMap> rows,
                                       const EncodableList* filters) {
  if (filters == nullptr || filters->empty()) {
    return rows;
  }

  std::vector<EncodableMap> filtered;
  for (auto& row : rows) {
    bool keep = true;
    for (const auto& filter_value : *filters) {
      const auto* filter = std::get_if<EncodableMap>(&filter_value);
      if (filter == nullptr) {
        continue;
      }
      const auto field = AsString(FindValue(*filter, "field"));
      const auto op = AsString(FindValue(*filter, "operator"));
      if (!field.has_value() || !op.has_value()) {
        continue;
      }

      const std::string actual = ValueAsString(row, *field);
      const auto* expected = FindValue(*filter, "value");

      if (*op == "equals") {
        if (actual != AsString(expected).value_or("")) {
          keep = false;
          break;
        }
      } else if (*op == "contains") {
        const std::string needle = Lower(AsString(expected).value_or(""));
        if (Lower(actual).find(needle) == std::string::npos) {
          keep = false;
          break;
        }
      } else if (*op == "inList") {
        const auto* expected_list = AsList(expected);
        if (expected_list != nullptr) {
          bool matched = false;
          for (const auto& item : *expected_list) {
            if (actual == AsString(&item).value_or("")) {
              matched = true;
              break;
            }
          }
          if (!matched) {
            keep = false;
            break;
          }
        }
      }
    }

    if (keep) {
      filtered.push_back(std::move(row));
    }
  }

  return filtered;
}

void ApplySort(std::vector<EncodableMap>* rows, const EncodableList* sort) {
  if (sort == nullptr || sort->empty()) {
    return;
  }
  const auto* first = std::get_if<EncodableMap>(&sort->front());
  if (first == nullptr) {
    return;
  }
  const auto field = AsString(FindValue(*first, "field"));
  if (!field.has_value()) {
    return;
  }
  const bool ascending = AsString(FindValue(*first, "direction")).value_or("ascending") !=
                         "descending";

  std::sort(rows->begin(), rows->end(), [&](const EncodableMap& lhs, const EncodableMap& rhs) {
    const auto lv = ValueAsString(lhs, *field);
    const auto rv = ValueAsString(rhs, *field);
    return ascending ? (lv < rv) : (lv > rv);
  });
}

EncodableList ApplyProjection(const std::vector<EncodableMap>& rows,
                              const EncodableList* projection) {
  EncodableList projected_rows;
  if (projection == nullptr || projection->empty()) {
    for (const auto& row : rows) {
      projected_rows.push_back(EncodableValue(row));
    }
    return projected_rows;
  }

  for (const auto& row : rows) {
    EncodableMap projected;
    for (const auto& key_value : *projection) {
      const auto key = AsString(&key_value).value_or("");
      auto it = row.find(EncodableValue(key));
      projected[EncodableValue(key)] =
          (it == row.end()) ? EncodableValue() : it->second;
    }
    projected_rows.push_back(EncodableValue(projected));
  }

  return projected_rows;
}

// Purpose: Name what one observer snapshot covers.
struct SnapshotRequest {
  // The directory a files or media observer watches.
  const std::filesystem::path& root;
  const std::string& domain;
  const StopRequested& stop_requested;
};

// Purpose: Take one observer snapshot of a domain's records.
// @param source names the domain, its root directory and the stop check.
// @returns Each record's id mapped to a fingerprint of its row.
// @throws std::runtime_error when a contacts or calendar store cannot be
// read, std::system_error when a file path cannot be converted to the system
// code page, and std::invalid_argument for a domain observe does not support.
Snapshot BuildSnapshotForDomain(const SnapshotRequest& source) {
  const std::string& domain = source.domain;
  if (domain == "files" || domain == "media") {
    return FingerprintRecords(ListRecords({.root = source.root,
                                           .media_only = domain == "media",
                                           .stop_requested = source.stop_requested}));
  }
  if (domain == "contacts" || domain == "calendar") {
    return FingerprintRecords(RowsOrThrow(domain == "contacts"
                                              ? ListContactRecords(source.stop_requested)
                                              : ListCalendarRecords(source.stop_requested)));
  }
  throw std::invalid_argument("simple_query: windows observe has no snapshot for domain " +
                              domain);
}

}  // namespace

// Purpose: Answer one engine's simple_query host calls on Windows.
// Create and destroy it on that engine's platform thread.
class NativeQueryHostApiImpl : public NativeQueryHostApi {
 public:
  // Purpose: Serve host calls and send observe events through one engine.
  // @param messenger is the engine's messenger; it must outlive this object.
  // @returns Nothing.
  // @throws Nothing.
  explicit NativeQueryHostApiImpl(flutter::BinaryMessenger* messenger)
      : flutter_api_(messenger),
        observers_({.send_event =
                        [this](const std::string& observer_id,
                               const EncodableMap& event) {
                          SendObserveEvent(observer_id, event);
                        },
                    .now_epoch_ms = &EpochMsNow}) {}

  ErrorOr<EncodableMap> GetCapabilities() override {
    const auto contacts_probe = ListContactRecords({});
    const auto calendar_probe = ListCalendarRecords({});

    EncodableList capabilities;
    capabilities.push_back(
        EncodableValue(Capability("contacts", !contacts_probe.error.has_value(), false,
                                  !contacts_probe.error.has_value(), false,
                                  contacts_probe.error)));
    capabilities.push_back(EncodableValue(Capability("media", true, true, true, true)));
    capabilities.push_back(EncodableValue(Capability("files", true, true, true, true)));
    capabilities.push_back(EncodableValue(Capability("calendar",
                                                     !calendar_probe.error.has_value(),
                                                     false,
                                                     !calendar_probe.error.has_value(),
                                                     false, calendar_probe.error)));
    capabilities.push_back(EncodableValue(
        Capability("messages", false, false, false, false,
                   "simple_query: messages is not supported on Windows")));
    capabilities.push_back(EncodableValue(
        Capability("calls", false, false, false, false,
                   "simple_query: calls is not supported on Windows")));
    capabilities.push_back(
        EncodableValue(Capability("platformSpecific", true, false, false, false)));

    EncodableMap extensions;
    extensions[EncodableValue("windows.contacts")] = EncodableValue(true);
    extensions[EncodableValue("windows.calendar")] = EncodableValue(true);
    extensions[EncodableValue("windows.storage")] = EncodableValue(true);

    EncodableMap result;
    result[EncodableValue("capabilities")] = EncodableValue(capabilities);
    result[EncodableValue("platformExtensions")] = EncodableValue(extensions);
    return result;
  }

  ErrorOr<EncodableMap> Query(const EncodableMap& request) override {
    const std::string domain = StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media" && domain != "contacts" &&
        domain != "calendar") {
      return FlutterError("not-supported",
                          "simple_query: query is not supported for domain " + domain +
                              " on Windows host");
    }

    std::vector<EncodableMap> rows;
    if (domain == "files" || domain == "media") {
      rows = ListRecords({.root = ResolveRootPath(request),
                          .media_only = domain == "media"});
    } else if (domain == "contacts") {
      auto contact_rows = ListContactRecords({});
      if (contact_rows.error.has_value()) {
        return FlutterError("unavailable", *contact_rows.error);
      }
      rows = std::move(contact_rows.rows);
    } else {
      auto calendar_rows = ListCalendarRecords({});
      if (calendar_rows.error.has_value()) {
        return FlutterError("unavailable", *calendar_rows.error);
      }
      rows = std::move(calendar_rows.rows);
    }
    rows = ApplyFilters(std::move(rows), AsList(FindValue(request, "filters")));
    ApplySort(&rows, AsList(FindValue(request, "sort")));

    const int64_t total_count = static_cast<int64_t>(rows.size());
    const EncodableMap* page = AsMap(FindValue(request, "page"));
    const int64_t offset = std::max<int64_t>(0, page == nullptr
                                                    ? 0
                                                    : AsInt(FindValue(*page, "offset")).value_or(0));
    const auto limit_opt = page == nullptr ? std::optional<int64_t>{}
                                           : AsInt(FindValue(*page, "limit"));

    const size_t start = static_cast<size_t>(std::min<int64_t>(offset, rows.size()));
    size_t end = rows.size();
    if (limit_opt.has_value()) {
      end = std::min(rows.size(), start + static_cast<size_t>(std::max<int64_t>(0, *limit_opt)));
    }

    std::vector<EncodableMap> paged_rows(rows.begin() + static_cast<long>(start),
                                         rows.begin() + static_cast<long>(end));

    EncodableMap result;
    result[EncodableValue("records")] =
        EncodableValue(ApplyProjection(paged_rows, AsList(FindValue(request, "projection"))));
    result[EncodableValue("totalCount")] = EncodableValue(total_count);
    if (end < rows.size()) {
      result[EncodableValue("nextOffset")] = EncodableValue(static_cast<int64_t>(end));
    }
    return result;
  }

  ErrorOr<EncodableMap> Mutate(const EncodableMap& request) override {
    const std::string domain = StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media") {
      return FlutterError("not-supported",
                          "simple_query: mutate is not supported for domain " + domain +
                              " on Windows host");
    }

    const std::string type = StringOr(request, "type", "");
    if (type == "insert") {
      const EncodableMap* values = AsMap(FindValue(request, "values"));
      if (values == nullptr) {
        return FlutterError("invalid-query", "simple_query: insert requires values");
      }
      const auto path = AsString(FindValue(*values, "path"));
      if (!path.has_value() || path->empty()) {
        return FlutterError("invalid-query", "simple_query: insert requires values.path");
      }

      std::error_code ec;
      const bool is_directory = BoolOr(*values, "isDirectory", false);
      if (is_directory) {
        std::filesystem::create_directories(*path, ec);
      } else {
        const auto file_path = std::filesystem::path(*path);
        std::filesystem::create_directories(file_path.parent_path(), ec);
        std::ofstream stream(*path, std::ios::binary);
        if (!stream.is_open()) {
          return FlutterError("unavailable", "simple_query: could not create output file");
        }
        stream << AsString(FindValue(*values, "content")).value_or("");
      }

      EncodableMap result;
      result[EncodableValue("affectedCount")] = EncodableValue(static_cast<int64_t>(1));
      result[EncodableValue("insertedId")] = EncodableValue(*path);
      return result;
    }

    if (type == "delete") {
      EncodableMap query_request;
      query_request[EncodableValue("domain")] = EncodableValue(domain);
      query_request[EncodableValue("filters")] =
          FindValue(request, "filters") == nullptr ? EncodableValue(EncodableList{})
                                                     : *FindValue(request, "filters");
      if (FindValue(request, "platformData") != nullptr) {
        query_request[EncodableValue("platformData")] = *FindValue(request, "platformData");
      }

      auto queried = Query(query_request);
      if (queried.has_error()) {
        return queried.error();
      }

      const auto* records = AsList(FindValue(queried.value(), "records"));
      int64_t deleted = 0;
      if (records != nullptr) {
        for (const auto& record_value : *records) {
          const auto* record = std::get_if<EncodableMap>(&record_value);
          if (record == nullptr) {
            continue;
          }
          const auto path = AsString(FindValue(*record, "path"));
          if (!path.has_value() || path->empty()) {
            continue;
          }
          std::error_code ec;
          deleted += static_cast<int64_t>(std::filesystem::remove_all(*path, ec));
        }
      }

      EncodableMap result;
      result[EncodableValue("affectedCount")] = EncodableValue(deleted);
      return result;
    }

    if (type == "update") {
      const EncodableMap* values = AsMap(FindValue(request, "values"));
      if (values == nullptr) {
        return FlutterError("invalid-query", "simple_query: update requires values");
      }

      std::set<std::string> target_paths;
      const auto explicit_path = AsString(FindValue(*values, "path"));
      const auto explicit_id = AsString(FindValue(*values, "id"));
      if (explicit_path.has_value() && !explicit_path->empty()) {
        target_paths.insert(*explicit_path);
      } else if (explicit_id.has_value() && !explicit_id->empty()) {
        target_paths.insert(*explicit_id);
      }

      if (target_paths.empty()) {
        EncodableMap query_request;
        query_request[EncodableValue("domain")] = EncodableValue(domain);
        query_request[EncodableValue("filters")] =
            FindValue(request, "filters") == nullptr ? EncodableValue(EncodableList{})
                                                       : *FindValue(request, "filters");
        if (FindValue(request, "platformData") != nullptr) {
          query_request[EncodableValue("platformData")] = *FindValue(request, "platformData");
        }
        auto queried = Query(query_request);
        if (queried.has_error()) {
          return queried.error();
        }
        const auto* records = AsList(FindValue(queried.value(), "records"));
        if (records != nullptr) {
          for (const auto& record_value : *records) {
            const auto* record = std::get_if<EncodableMap>(&record_value);
            if (record == nullptr) {
              continue;
            }
            const auto path = AsString(FindValue(*record, "path"));
            if (path.has_value() && !path->empty()) {
              target_paths.insert(*path);
            }
          }
        }
      }

      int64_t updated = 0;
      for (const auto& original_path : target_paths) {
        std::error_code ec;
        std::filesystem::path effective_path(original_path);
        if (!std::filesystem::exists(effective_path, ec)) {
          continue;
        }

        bool changed = false;
        const auto new_path = AsString(FindValue(*values, "newPath"));
        if (new_path.has_value() && !new_path->empty() && *new_path != original_path) {
          const std::filesystem::path next_path(*new_path);
          std::filesystem::create_directories(next_path.parent_path(), ec);
          ec.clear();
          std::filesystem::rename(effective_path, next_path, ec);
          if (!ec) {
            effective_path = next_path;
            changed = true;
          }
        }

        const bool is_directory = std::filesystem::is_directory(effective_path, ec);
        if (!is_directory) {
          const auto* bytes_value = FindValue(*values, "bytes");
          if (bytes_value != nullptr) {
            if (const auto* bytes = std::get_if<std::vector<uint8_t>>(bytes_value)) {
              std::ofstream stream(effective_path.string(), std::ios::binary | std::ios::trunc);
              if (!stream.is_open()) {
                return FlutterError("unavailable", "simple_query: could not open file for update");
              }
              stream.write(reinterpret_cast<const char*>(bytes->data()),
                           static_cast<std::streamsize>(bytes->size()));
              changed = true;
            } else {
              return FlutterError(
                  "invalid-query",
                  "simple_query: update expects values.bytes as Uint8List when provided");
            }
          } else if (const auto* content_value = FindValue(*values, "content");
                     content_value != nullptr) {
            std::ofstream stream(effective_path.string(), std::ios::binary | std::ios::trunc);
            if (!stream.is_open()) {
              return FlutterError("unavailable", "simple_query: could not open file for update");
            }
            stream << AsString(content_value).value_or("");
            changed = true;
          }
        }

        if (changed) {
          updated += 1;
        }
      }

      EncodableMap result;
      result[EncodableValue("affectedCount")] = EncodableValue(updated);
      return result;
    }

    return FlutterError("invalid-query", "simple_query: unknown mutation type " + type);
  }

  ErrorOr<EncodableMap> Batch(const EncodableMap& request) override {
    const auto* operations = AsList(FindValue(request, "operations"));
    if (operations == nullptr) {
      return FlutterError("invalid-query", "simple_query: batch requires operations");
    }

    EncodableList results;
    for (const auto& op_value : *operations) {
      const auto* op = std::get_if<EncodableMap>(&op_value);
      if (op == nullptr) {
        return FlutterError("invalid-query", "simple_query: batch operation must be a map");
      }
      EncodableMap merged = *op;
      if (FindValue(*op, "platformData") == nullptr &&
          FindValue(request, "platformData") != nullptr) {
        merged[EncodableValue("platformData")] = *FindValue(request, "platformData");
      }

      auto result = Mutate(merged);
      if (result.has_error()) {
        return result.error();
      }
      results.push_back(EncodableValue(result.value()));
    }

    EncodableMap response;
    response[EncodableValue("results")] = EncodableValue(results);
    return response;
  }

  ErrorOr<std::string> ObserveStart(const EncodableMap& request) override {
    const std::string domain = StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media" && domain != "contacts" &&
        domain != "calendar") {
      return FlutterError("not-supported",
                          "simple_query: observe is not supported for domain " + domain +
                              " on Windows host");
    }

    if (!observers_.IsReady()) {
      return FlutterError("unavailable",
                          "simple_query: windows observe could not create its "
                          "platform-thread message window");
    }

    int64_t interval_ms = AsInt(FindValue(request, "pollingIntervalMs")).value_or(1000);
    interval_ms = std::max<int64_t>(250, interval_ms);

    // Only files and media read a root. It is resolved once, here, so the
    // worker keeps only what its snapshots read.
    std::filesystem::path root;
    if (domain == "files" || domain == "media") {
      try {
        root = ResolveRootPath(request);
      } catch (const std::filesystem::filesystem_error& error) {
        return FlutterError("unavailable",
                            std::string("simple_query: windows observe could not "
                                        "resolve its root directory: ") +
                                error.what());
      }
    }
    ObserverSpec spec{
        .domain = domain,
        .polling_interval = std::chrono::milliseconds(interval_ms),
        .take_snapshot =
            [root, domain](const StopRequested& stop_requested) {
              return BuildSnapshotForDomain({.root = root,
                                             .domain = domain,
                                             .stop_requested = stop_requested});
            },
    };
    try {
      return observers_.Start(std::move(spec));
    } catch (const std::system_error& error) {
      return FlutterError("unavailable",
                          std::string("simple_query: windows observe could not "
                                      "start its polling thread: ") +
                              error.what());
    }
  }

  std::optional<FlutterError> ObserveStop(const std::string& observer_id) override {
    observers_.Stop(observer_id);
    return std::nullopt;
  }

  ErrorOr<EncodableMap> OpenBinary(const EncodableMap& request) override {
    const std::string domain = StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media") {
      return FlutterError("not-supported",
                          "simple_query: openBinary is not supported for domain " + domain +
                              " on Windows host");
    }

    std::optional<std::string> path;
    const EncodableMap* platform_data = AsMap(FindValue(request, "platformData"));
    if (platform_data != nullptr) {
      path = AsString(FindValue(*platform_data, "path"));
    }
    if (!path.has_value()) {
      path = AsString(FindValue(request, "recordId"));
    }

    if (!path.has_value() || path->empty()) {
      return FlutterError("invalid-query", "simple_query: openBinary requires recordId or platformData.path");
    }

    std::error_code ec;
    if (!std::filesystem::exists(*path, ec)) {
      return FlutterError("unavailable", "simple_query: binary resource was not found");
    }

    const auto handle = std::string("win_handle_") + std::to_string(++handle_counter_);
    open_handles_[handle] = *path;

    EncodableMap result;
    result[EncodableValue("handleId")] = EncodableValue(handle);
    result[EncodableValue("localPath")] = EncodableValue(*path);
    result[EncodableValue("mimeType")] = EncodableValue(MimeFromPath(*path));
    if (std::filesystem::is_regular_file(*path, ec)) {
      result[EncodableValue("size")] =
          EncodableValue(static_cast<int64_t>(std::filesystem::file_size(*path, ec)));
    }
    EncodableMap metadata;
    metadata[EncodableValue("source")] = EncodableValue("windows-host");
    result[EncodableValue("metadata")] = EncodableValue(metadata);
    return result;
  }

  std::optional<FlutterError> CloseBinary(const std::string& handle_id) override {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  ErrorOr<std::optional<EncodableMap>> CallExtension(
      const std::string& namespaze, const std::string& method,
      const EncodableMap* args) override {
    if (namespaze == "windows.contacts" && method == "listStores") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: windows.contacts.listStores does not accept arguments");
      }
      EncodableList stores;
      try {
        stores = ListContactStores();
      } catch (const winrt::hresult_error& error) {
        return FlutterError("unavailable",
                            "simple_query: windows contacts backend unavailable - " +
                                winrt::to_string(error.message()));
      } catch (const std::exception& error) {
        return FlutterError("unavailable",
                            "simple_query: windows contacts backend unavailable - " +
                                std::string(error.what()));
      }

      EncodableMap response;
      response[EncodableValue("stores")] = EncodableValue(stores);
      return std::make_optional(std::move(response));
    }

    if (namespaze == "windows.calendar" && method == "listCalendars") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: windows.calendar.listCalendars does not accept arguments");
      }
      EncodableList calendars;
      try {
        calendars = ListCalendarStores();
      } catch (const winrt::hresult_error& error) {
        return FlutterError("unavailable",
                            "simple_query: windows calendar backend unavailable - " +
                                winrt::to_string(error.message()));
      } catch (const std::exception& error) {
        return FlutterError("unavailable",
                            "simple_query: windows calendar backend unavailable - " +
                                std::string(error.what()));
      }
      EncodableMap response;
      response[EncodableValue("calendars")] = EncodableValue(calendars);
      return std::make_optional(std::move(response));
    }

    if (namespaze == "windows.storage" && method == "resolveKnownFolders") {
      bool include_temp = true;
      if (args != nullptr) {
        if (const auto* include_temp_value = FindValue(*args, "includeTemp")) {
          if (const auto* include_temp_bool = std::get_if<bool>(include_temp_value)) {
            include_temp = *include_temp_bool;
          } else {
            return FlutterError(
                "invalid-query",
                "simple_query: windows.storage.resolveKnownFolders expects includeTemp as bool");
          }
        }
      }

      EncodableList folders;
      EncodableMap cwd;
      cwd[EncodableValue("id")] = EncodableValue("current");
      cwd[EncodableValue("path")] = EncodableValue(std::filesystem::current_path().string());
      folders.push_back(EncodableValue(cwd));

      if (include_temp) {
        EncodableMap temp;
        temp[EncodableValue("id")] = EncodableValue("temp");
        temp[EncodableValue("path")] =
            EncodableValue(std::filesystem::temp_directory_path().string());
        folders.push_back(EncodableValue(temp));
      }

      EncodableMap response;
      response[EncodableValue("folders")] = EncodableValue(folders);
      return std::make_optional(std::move(response));
    }

    if (namespaze == "windows.storage" && method == "listLibraries") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: windows.storage.listLibraries does not accept arguments");
      }
      EncodableList libraries;
      libraries.push_back(EncodableValue("documents"));
      libraries.push_back(EncodableValue("music"));
      libraries.push_back(EncodableValue("pictures"));
      libraries.push_back(EncodableValue("videos"));

      EncodableMap response;
      response[EncodableValue("libraries")] = EncodableValue(libraries);
      return std::make_optional(std::move(response));
    }

    return FlutterError("not-supported",
                        "simple_query: " + namespaze + "." + method +
                            " is not supported on Windows host");
  }

 private:
  // Purpose: Send one observe event to Dart. Runs on the platform thread.
  // A run of rejected events is logged once, because a Dart side that stopped
  // listening rejects every event after it. An accepted event ends the run,
  // so the next rejection is logged again.
  // @param observer_id is the observer the event belongs to.
  // @param event is the change event map.
  // @returns Nothing.
  // @throws Nothing.
  void SendObserveEvent(const std::string& observer_id, const EncodableMap& event) {
    flutter_api_.OnObserveEvent(
        observer_id, event,
        [reported = rejected_event_reported_]() { *reported = false; },
        [observer_id, reported = rejected_event_reported_](const FlutterError& error) {
          if (*reported) {
            return;
          }
          *reported = true;
          WriteLog({LogLevel::kWarning,
                    "observe_event_rejected",
                    {{"observer_id", observer_id},
                     {"code", error.code()},
                     {"message", error.message()},
                     {"next_log", "after an event is accepted"}}});
        });
  }

  NativeQueryFlutterApi flutter_api_;
  // Shared with pending replies, which can arrive after this object is gone.
  // Read and written only on the platform thread.
  std::shared_ptr<bool> rejected_event_reported_ = std::make_shared<bool>(false);
  int64_t handle_counter_ = 0;
  std::map<std::string, std::string> open_handles_;
  // Declared after flutter_api_ so it is destroyed first: its destructor
  // stops every observer before the API it sends through goes away.
  PollingObservers observers_;
};

// Each engine's registrar owns its own plugin and host API, so every engine's
// events go to its own messenger.
void SimpleQueryWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  registrar->AddPlugin(
      std::make_unique<SimpleQueryWindowsPlugin>(registrar->messenger()));
}

SimpleQueryWindowsPlugin::SimpleQueryWindowsPlugin(
    flutter::BinaryMessenger* messenger)
    : messenger_(messenger),
      host_api_(std::make_unique<NativeQueryHostApiImpl>(messenger)) {
  NativeQueryHostApi::SetUp(messenger_, host_api_.get());
}

// The registrar destroys its plugins while the messenger is still alive, so
// the handlers are removed and every observer stops before the engine goes.
SimpleQueryWindowsPlugin::~SimpleQueryWindowsPlugin() {
  NativeQueryHostApi::SetUp(messenger_, nullptr);
}

}  // namespace simple_query_windows
