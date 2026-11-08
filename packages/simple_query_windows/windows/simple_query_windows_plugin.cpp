#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "domain_snapshots.h"
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

// Purpose: Look up one key of a request map.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value, or null when the key is absent.
// @throws Nothing.
const EncodableValue* FindValue(const EncodableMap& map, const std::string& key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &it->second;
}

// Purpose: Read a scalar request value as text.
// @param value is the value, or null.
// @returns The value as text, or nothing when it is absent or not a scalar.
// @throws Nothing.
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

// Purpose: Read one key of a map as text, with a default for a missing key.
// @param map is the map to search.
// @param key is the key's text.
// @param fallback is returned when the key is absent or not a scalar.
// @returns The value as text, or fallback.
// @throws Nothing.
std::string StringOr(const EncodableMap& map, const std::string& key,
                     const std::string& fallback) {
  const auto* value = FindValue(map, key);
  const auto parsed = AsString(value);
  return parsed.value_or(fallback);
}

// Purpose: Read a numeric request value as an integer.
// @param value is the value, or null.
// @returns The value, truncated when it is a double, or nothing when it is
// absent or not a number.
// @throws Nothing.
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

// Purpose: Read one key of a map as a bool, with a default for a missing key.
// @param map is the map to search.
// @param key is the key's text.
// @param fallback is returned when the key is absent or not a bool.
// @returns The value, or fallback.
// @throws Nothing.
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

// Purpose: View a request value as a map.
// @param value is the value, or null.
// @returns The map, or null when the value is absent or not a map.
// @throws Nothing.
const EncodableMap* AsMap(const EncodableValue* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return std::get_if<EncodableMap>(value);
}

// Purpose: View a request value as a list.
// @param value is the value, or null.
// @returns The list, or null when the value is absent or not a list.
// @throws Nothing.
const EncodableList* AsList(const EncodableValue* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return std::get_if<EncodableList>(value);
}

// Purpose: Read the wall clock for event timestamps.
// @returns Milliseconds since the Unix epoch.
// @throws Nothing.
int64_t EpochMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Purpose: Read one field of a row as text for filtering and sorting.
// @param row is the record row.
// @param key is the field name.
// @returns The field as text, or empty when it is absent or not a scalar.
// @throws Nothing.
std::string ValueAsString(const EncodableMap& row, const std::string& key) {
  auto it = row.find(EncodableValue(key));
  if (it == row.end()) {
    return "";
  }
  return AsString(&it->second).value_or("");
}

// Purpose: Build the capability entry Dart reads for one domain.
// @param domain is the domain name.
// @param can_read states whether the domain can be queried.
// @param can_write states whether the domain can be mutated.
// @param can_observe states whether the domain can be observed.
// @param can_stream states whether the domain's binaries can be opened.
// @param reason explains a missing capability, if any.
// @returns The capability map.
// @throws Nothing.
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

// Purpose: Find the directory a files or media request reads.
// @param request is the request; platformData.rootPath names the directory.
// @param current_directory reads the working directory, used when the
// request names none.
// @returns The named directory, or the working directory.
// @throws std::system_error when the named path cannot be converted to the
// system code page, or the working directory cannot be read.
std::filesystem::path ResolveRootPath(
    const EncodableMap& request,
    const std::function<std::filesystem::path()>& current_directory) {
  const EncodableMap* platform_data = AsMap(FindValue(request, "platformData"));
  if (platform_data != nullptr) {
    const auto root = AsString(FindValue(*platform_data, "rootPath"));
    if (root.has_value() && !root->empty()) {
      return std::filesystem::path(*root);
    }
  }
  return current_directory();
}

// Purpose: Keep the rows that pass every filter of a query.
// @param rows are the rows to filter.
// @param filters are the query's filter maps, or null for none. A filter
// without a field or operator, or with an unknown operator, is ignored.
// @returns The rows that pass, in their original order.
// @throws Nothing.
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

// Purpose: Sort rows by the query's first sort key, comparing values as text.
// @param rows are the rows to sort in place.
// @param sort is the query's sort list, or null for none.
// @returns Nothing.
// @throws Nothing.
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

// Purpose: Keep only the projected fields of each row.
// @param rows are the rows to project.
// @param projection lists the field names, or null for every field.
// @returns The projected rows; a missing field is present as null.
// @throws Nothing.
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

}  // namespace

// Purpose: Answer one engine's simple_query host calls on Windows.
// Create and destroy it on that engine's platform thread.
class NativeQueryHostApiImpl : public NativeQueryHostApi {
 public:
  // Purpose: Serve host calls and send observe events through one engine.
  // @param messenger is the engine's messenger; it must outlive this object.
  // @param effects supplies the environment reads.
  // @returns Nothing.
  // @throws Nothing.
  NativeQueryHostApiImpl(flutter::BinaryMessenger* messenger,
                         PluginEffects effects)
      : current_directory_(std::move(effects.current_directory)),
        flutter_api_(messenger),
        observers_({.send_event =
                        [this](const std::string& observer_id,
                               const EncodableMap& event) {
                          SendObserveEvent(observer_id, event);
                        },
                    .now_epoch_ms = &EpochMsNow}) {}

  // Purpose: Report what each domain supports on this machine.
  // @returns The capabilities and the platform extensions.
  // @throws Nothing; an unreadable store is reported in its capability.
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

  // Purpose: Answer one query for files, media, contacts or calendar.
  // @param request names the domain, filters, sort, page and projection.
  // @returns One page of records with the total count, or an error for an
  // unsupported domain or an unreadable store.
  // @throws std::system_error when a file path cannot be converted to the
  // system code page.
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
      rows = ListRecords({.root = ResolveRootPath(request, current_directory_),
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

  // Purpose: Insert, update or delete files or media.
  // @param request names the domain, the mutation type and its values.
  // @returns The affected count, or an error for an invalid request.
  // @throws std::system_error when a file path cannot be converted to the
  // system code page.
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

  // Purpose: Run several mutations in order, stopping at the first failure.
  // @param request holds the operations and a shared platformData.
  // @returns Each mutation's result, or the first error.
  // @throws What Mutate throws.
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

  // Purpose: Start polling one domain and sending its changes to Dart.
  // @param request names the domain, the polling interval and the root.
  // @returns The new observer's id, or an unavailable error when the root
  // cannot be resolved or the worker cannot start.
  // @throws Nothing.
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
        root = ResolveRootPath(request, current_directory_);
      } catch (const std::system_error& error) {
        // Covers an unreadable working directory and a root the system code
        // page cannot represent.
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

  // Purpose: Stop one observer. An unknown id does nothing.
  // @param observer_id is the id ObserveStart returned.
  // @returns Nothing on success.
  // @throws Nothing.
  std::optional<FlutterError> ObserveStop(const std::string& observer_id) override {
    observers_.Stop(observer_id);
    return std::nullopt;
  }

  // Purpose: Describe a file so Dart can read its bytes.
  // @param request names the file by platformData.path or recordId.
  // @returns A handle, the file's path, MIME type and size, or an error.
  // @throws Nothing.
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

  // Purpose: Forget a handle OpenBinary returned.
  // @param handle_id is the handle to forget.
  // @returns Nothing on success.
  // @throws Nothing.
  std::optional<FlutterError> CloseBinary(const std::string& handle_id) override {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  // Purpose: Run one Windows-specific extension method.
  // @param namespaze is the extension namespace, such as windows.storage.
  // @param method is the method name.
  // @param args are the method's arguments, or null.
  // @returns The method's result, or an error for an unknown method, invalid
  // arguments or an unreadable store.
  // @throws std::system_error when the working directory or temp directory
  // cannot be read.
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
      cwd[EncodableValue("path")] = EncodableValue(current_directory_().string());
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

  // Read only on the platform thread.
  const std::function<std::filesystem::path()> current_directory_;
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
// Purpose: Create this engine's plugin and hand it to the registrar.
// @param registrar is the engine's plugin registrar.
// @returns Nothing.
// @throws Nothing.
void SimpleQueryWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  registrar->AddPlugin(
      std::make_unique<SimpleQueryWindowsPlugin>(registrar->messenger()));
}

// Purpose: Start answering host API calls on one engine's messenger.
// @param messenger is the engine's messenger; it must outlive the plugin.
// @param effects supplies the environment reads.
// @returns Nothing.
// @throws Nothing.
SimpleQueryWindowsPlugin::SimpleQueryWindowsPlugin(
    flutter::BinaryMessenger* messenger, PluginEffects effects)
    : messenger_(messenger),
      host_api_(std::make_unique<NativeQueryHostApiImpl>(messenger,
                                                         std::move(effects))) {
  NativeQueryHostApi::SetUp(messenger_, host_api_.get());
}

// The registrar destroys its plugins while the messenger is still alive, so
// the handlers are removed and every observer stops before the engine goes.
// Purpose: Remove this engine's handlers, then stop every observer.
// @returns Nothing.
// @throws Nothing.
SimpleQueryWindowsPlugin::~SimpleQueryWindowsPlugin() {
  NativeQueryHostApi::SetUp(messenger_, nullptr);
}

}  // namespace simple_query_windows
