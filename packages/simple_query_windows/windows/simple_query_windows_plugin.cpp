#include "include/simple_query_windows/simple_query_windows_plugin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <flutter/plugin_registrar_windows.h>
#include <winrt/Windows.ApplicationModel.Appointments.h>
#include <winrt/Windows.ApplicationModel.Contacts.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "native_query.g.h"

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

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string MimeFromPath(const std::filesystem::path& path) {
  const std::string ext = Lower(path.extension().string());
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".gif") return "image/gif";
  if (ext == ".heic") return "image/heic";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".aac") return "audio/aac";
  if (ext == ".flac") return "audio/flac";
  if (ext == ".txt") return "text/plain";
  if (ext == ".json") return "application/json";
  return "application/octet-stream";
}

bool IsMediaMime(const std::string& mime) {
  return mime.rfind("image/", 0) == 0 || mime.rfind("video/", 0) == 0 ||
         mime.rfind("audio/", 0) == 0;
}

std::string MediaType(const std::string& mime) {
  if (mime.rfind("image/", 0) == 0) return "image";
  if (mime.rfind("video/", 0) == 0) return "video";
  if (mime.rfind("audio/", 0) == 0) return "audio";
  return "other";
}

int64_t EpochMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void EnsureWinRtApartment() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
      // Already initialized with a different apartment type.
    }
  });
}

int64_t ToUnixEpochMs(winrt::Windows::Foundation::DateTime value) {
  constexpr int64_t kWinRtToUnixEpochTicks = 116444736000000000LL;
  const int64_t ticks = value.time_since_epoch().count();
  return (ticks - kWinRtToUnixEpochTicks) / 10000;
}

struct WinRtRowsResult {
  std::vector<EncodableMap> rows;
  std::optional<std::string> error;
};

WinRtRowsResult ListContactRecords() {
  using namespace winrt::Windows::ApplicationModel::Contacts;
  WinRtRowsResult result;
  try {
    EnsureWinRtApartment();

    const auto store =
        ContactManager::RequestStoreAsync(ContactStoreAccessType::AllContactsReadOnly).get();
    const auto contacts = store.FindContactsAsync().get();

    for (const auto& contact : contacts) {
      std::string id = winrt::to_string(contact.Id());
      if (id.empty()) {
        continue;
      }

      std::string display_name = winrt::to_string(contact.DisplayName());
      if (display_name.empty()) {
        display_name = winrt::to_string(contact.FirstName());
        const auto last_name = winrt::to_string(contact.LastName());
        if (!last_name.empty()) {
          if (!display_name.empty()) {
            display_name += " ";
          }
          display_name += last_name;
        }
      }

      EncodableList phones;
      for (const auto& phone : contact.Phones()) {
        phones.push_back(EncodableValue(winrt::to_string(phone.Number())));
      }

      EncodableList emails;
      for (const auto& email : contact.Emails()) {
        emails.push_back(EncodableValue(winrt::to_string(email.Address())));
      }

      EncodableMap row;
      row[EncodableValue("id")] = EncodableValue(id);
      row[EncodableValue("displayName")] = EncodableValue(display_name);
      row[EncodableValue("phones")] = EncodableValue(phones);
      row[EncodableValue("emails")] = EncodableValue(emails);

      if (contact.JobInfo().Size() > 0) {
        const auto company = winrt::to_string(contact.JobInfo().GetAt(0).CompanyName());
        if (!company.empty()) {
          row[EncodableValue("organization")] = EncodableValue(company);
        }
      }

      result.rows.push_back(std::move(row));
    }
  } catch (const winrt::hresult_error& error) {
    result.error = "simple_query: windows contacts backend unavailable - " +
                   winrt::to_string(error.message());
  } catch (const std::exception& error) {
    result.error = "simple_query: windows contacts backend unavailable - " +
                   std::string(error.what());
  }
  return result;
}

WinRtRowsResult ListCalendarRecords() {
  using namespace winrt::Windows::ApplicationModel::Appointments;
  WinRtRowsResult result;
  try {
    EnsureWinRtApartment();

    const auto store =
        AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly)
            .get();
    const auto calendars = store.FindAppointmentCalendarsAsync().get();

    const auto now = winrt::clock::now();
    winrt::Windows::Foundation::TimeSpan lookback{};
    lookback.Duration = std::chrono::duration_cast<
                            winrt::Windows::Foundation::TimeSpan::duration>(
                            std::chrono::hours(24 * 365))
                            .count();
    const auto start = now - lookback;
    winrt::Windows::Foundation::TimeSpan range{};
    range.Duration = std::chrono::duration_cast<
                         winrt::Windows::Foundation::TimeSpan::duration>(
                         std::chrono::hours(24 * 365 * 2))
                         .count();

    for (const auto& calendar : calendars) {
      const auto appointments = calendar.FindAppointmentsAsync(start, range).get();
      const std::string calendar_id = winrt::to_string(calendar.LocalId());
      for (const auto& appointment : appointments) {
        const auto start_time = appointment.StartTime();
        const auto end_time = start_time + appointment.Duration();
        const int64_t start_ms = ToUnixEpochMs(start_time);
        const int64_t end_ms = ToUnixEpochMs(end_time);

        std::string appointment_id = winrt::to_string(appointment.LocalId());
        if (appointment_id.empty()) {
          appointment_id = calendar_id + ":" + std::to_string(start_ms);
        }

        EncodableMap row;
        row[EncodableValue("id")] = EncodableValue(appointment_id);
        row[EncodableValue("title")] = EncodableValue(winrt::to_string(appointment.Subject()));
        row[EncodableValue("startAt")] = EncodableValue(std::to_string(start_ms));
        row[EncodableValue("endAt")] = EncodableValue(std::to_string(end_ms));
        row[EncodableValue("isAllDay")] = EncodableValue(appointment.AllDay());
        row[EncodableValue("calendarId")] = EncodableValue(calendar_id);
        row[EncodableValue("updatedAt")] = EncodableValue(std::to_string(start_ms));
        result.rows.push_back(std::move(row));
      }
    }
  } catch (const winrt::hresult_error& error) {
    result.error = "simple_query: windows calendar backend unavailable - " +
                   winrt::to_string(error.message());
  } catch (const std::exception& error) {
    result.error = "simple_query: windows calendar backend unavailable - " +
                   std::string(error.what());
  }
  return result;
}

EncodableList ListContactStores() {
  using namespace winrt::Windows::ApplicationModel::Contacts;
  EnsureWinRtApartment();

  const auto store =
      ContactManager::RequestStoreAsync(ContactStoreAccessType::AllContactsReadOnly).get();
  const auto lists = store.FindContactListsAsync().get();

  EncodableList stores;
  for (const auto& list : lists) {
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(winrt::to_string(list.Id()));
    row[EncodableValue("name")] = EncodableValue(winrt::to_string(list.DisplayName()));
    stores.push_back(EncodableValue(row));
  }
  return stores;
}

EncodableList ListCalendarStores() {
  using namespace winrt::Windows::ApplicationModel::Appointments;
  EnsureWinRtApartment();

  const auto store =
      AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly)
          .get();
  const auto calendars = store.FindAppointmentCalendarsAsync().get();

  EncodableList rows;
  for (const auto& calendar : calendars) {
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(winrt::to_string(calendar.LocalId()));
    row[EncodableValue("title")] = EncodableValue(winrt::to_string(calendar.DisplayName()));
    rows.push_back(EncodableValue(row));
  }
  return rows;
}

int64_t ModifiedEpochMs(const std::filesystem::directory_entry& entry) {
  auto ticks = entry.last_write_time().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count();
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

std::vector<EncodableMap> ListRecords(const std::filesystem::path& root,
                                      bool media_only) {
  std::vector<EncodableMap> rows;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return rows;
  }

  for (auto it = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }

    const auto& entry = *it;
    const auto path = entry.path();
    const bool is_dir = entry.is_directory(ec);
    const std::string mime = MimeFromPath(path);

    if (media_only) {
      if (is_dir || !IsMediaMime(mime)) {
        continue;
      }
      EncodableMap row;
      row[EncodableValue("id")] = EncodableValue(path.string());
      row[EncodableValue("uriOrPath")] = EncodableValue(path.string());
      row[EncodableValue("mediaType")] = EncodableValue(MediaType(mime));
      row[EncodableValue("mimeType")] = EncodableValue(mime);
      if (entry.is_regular_file(ec)) {
        row[EncodableValue("size")] = EncodableValue(static_cast<int64_t>(entry.file_size(ec)));
      }
      const auto modified = ModifiedEpochMs(entry);
      row[EncodableValue("createdAt")] = EncodableValue(std::to_string(modified));
      row[EncodableValue("modifiedAt")] = EncodableValue(std::to_string(modified));
      rows.push_back(std::move(row));
      continue;
    }

    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(path.string());
    row[EncodableValue("path")] = EncodableValue(path.string());
    row[EncodableValue("name")] = EncodableValue(path.filename().string());
    row[EncodableValue("isDirectory")] = EncodableValue(is_dir);
    if (entry.is_regular_file(ec)) {
      row[EncodableValue("size")] = EncodableValue(static_cast<int64_t>(entry.file_size(ec)));
    }
    const auto modified = ModifiedEpochMs(entry);
    row[EncodableValue("modifiedAt")] = EncodableValue(std::to_string(modified));
    row[EncodableValue("mimeType")] = EncodableValue(mime);
    row[EncodableValue("type")] = EncodableValue(is_dir ? "directory" : "file");
    row[EncodableValue("extension")] = EncodableValue(path.extension().string());
    row[EncodableValue("modifiedEpochMs")] = EncodableValue(modified);
    rows.push_back(std::move(row));
  }

  return rows;
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

using Snapshot = std::map<std::string, int64_t>;

Snapshot BuildSnapshotForDomain(const EncodableMap& request,
                                const std::string& domain) {
  Snapshot snapshot;
  std::vector<EncodableMap> rows;

  if (domain == "files" || domain == "media") {
    rows = ListRecords(ResolveRootPath(request), domain == "media");
  } else if (domain == "contacts") {
    rows = ListContactRecords().rows;
  } else if (domain == "calendar") {
    rows = ListCalendarRecords().rows;
  }

  for (const auto& row : rows) {
    const auto id = AsString(FindValue(row, "id"));
    if (!id.has_value()) {
      continue;
    }
    int64_t modified = AsInt(FindValue(row, "modifiedEpochMs"))
                           .value_or(AsInt(FindValue(row, "updatedAt"))
                                         .value_or(AsInt(FindValue(row, "startAt"))
                                                       .value_or(0)));
    snapshot[*id] = modified;
  }
  return snapshot;
}

EncodableList ChangedIds(const Snapshot& lhs, const Snapshot& rhs) {
  std::set<std::string> ids;
  for (const auto& [id, _] : lhs) {
    const auto it = rhs.find(id);
    if (it == rhs.end() || it->second != lhs.at(id)) {
      ids.insert(id);
    }
  }
  for (const auto& [id, _] : rhs) {
    const auto it = lhs.find(id);
    if (it == lhs.end()) {
      ids.insert(id);
    }
  }

  EncodableList values;
  for (const auto& id : ids) {
    values.push_back(EncodableValue(id));
  }
  return values;
}

struct ObserverState {
  std::atomic<bool> active{true};
  std::thread worker;
};

}  // namespace

class NativeQueryHostApiImpl : public NativeQueryHostApi {
 public:
  explicit NativeQueryHostApiImpl(flutter::BinaryMessenger* messenger)
      : flutter_api_(messenger) {}

  ~NativeQueryHostApiImpl() override { ShutdownObservers(); }

  ErrorOr<EncodableMap> GetCapabilities() override {
    const auto contacts_probe = ListContactRecords();
    const auto calendar_probe = ListCalendarRecords();

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
      rows = ListRecords(ResolveRootPath(request), domain == "media");
    } else if (domain == "contacts") {
      auto contact_rows = ListContactRecords();
      if (contact_rows.error.has_value()) {
        return FlutterError("unavailable", *contact_rows.error);
      }
      rows = std::move(contact_rows.rows);
    } else {
      auto calendar_rows = ListCalendarRecords();
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

    int64_t interval_ms = AsInt(FindValue(request, "pollingIntervalMs")).value_or(1000);
    interval_ms = std::max<int64_t>(250, interval_ms);

    const std::string observer_id =
        std::string("windows_observer_") + std::to_string(++observer_counter_);
    auto state = std::make_unique<ObserverState>();
    auto* state_ptr = state.get();

    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      observers_[observer_id] = std::move(state);
    }

    state_ptr->worker = std::thread(
        [this, observer_id, domain, request, interval_ms, state_ptr]() {
      Snapshot previous = BuildSnapshotForDomain(request, domain);
      while (state_ptr->active.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        if (!state_ptr->active.load()) {
          break;
        }
        Snapshot current = BuildSnapshotForDomain(request, domain);
        if (current == previous) {
          continue;
        }

        EncodableMap event;
        event[EncodableValue("domain")] = EncodableValue(domain);
        event[EncodableValue("changeType")] = EncodableValue("unknown");
        event[EncodableValue("timestamp")] =
            EncodableValue(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count()));
        event[EncodableValue("ids")] = EncodableValue(ChangedIds(previous, current));
        event[EncodableValue("source")] = EncodableValue("windows-host");
        previous = std::move(current);

        flutter_api_.OnObserveEvent(
            observer_id, event, []() {}, [](const FlutterError&) {});
      }
    });

    return observer_id;
  }

  std::optional<FlutterError> ObserveStop(const std::string& observer_id) override {
    std::unique_ptr<ObserverState> state;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      auto it = observers_.find(observer_id);
      if (it == observers_.end()) {
        return std::nullopt;
      }
      state = std::move(it->second);
      observers_.erase(it);
    }
    state->active.store(false);
    if (state->worker.joinable()) {
      state->worker.join();
    }
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
      return response;
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
      return response;
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
      return response;
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
      return response;
    }

    return FlutterError("not-supported",
                        "simple_query: " + namespaze + "." + method +
                            " is not supported on Windows host");
  }

 private:
  void ShutdownObservers() {
    std::vector<std::unique_ptr<ObserverState>> states;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      for (auto& item : observers_) {
        states.push_back(std::move(item.second));
      }
      observers_.clear();
    }
    for (auto& state : states) {
      state->active.store(false);
      if (state->worker.joinable()) {
        state->worker.join();
      }
    }
  }

  NativeQueryFlutterApi flutter_api_;
  int64_t handle_counter_ = 0;
  int64_t observer_counter_ = 0;
  std::map<std::string, std::string> open_handles_;
  std::map<std::string, std::unique_ptr<ObserverState>> observers_;
  std::mutex observers_mutex_;
};

}  // namespace

void SimpleQueryWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<SimpleQueryWindowsPlugin>();
  registrar->AddPlugin(std::move(plugin));

  static std::unique_ptr<NativeQueryHostApiImpl> host_api;
  if (!host_api) {
    host_api = std::make_unique<NativeQueryHostApiImpl>(registrar->messenger());
  }
  NativeQueryHostApi::SetUp(registrar->messenger(), host_api.get());
}

SimpleQueryWindowsPlugin::SimpleQueryWindowsPlugin() {}

SimpleQueryWindowsPlugin::~SimpleQueryWindowsPlugin() {}

}  // namespace simple_query_windows
