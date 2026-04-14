#include "include/simple_query_linux/simple_query_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>

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
#include <string>
#include <thread>
#include <vector>

#include <gtk/gtk.h>

#ifdef HAS_LIBEBOOK
#include <libebook/libebook.h>
#endif

#ifdef HAS_LIBECAL
#include <libecal/libecal.h>
#endif

#include "native_query.g.h"

#define SIMPLE_QUERY_LINUX_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), simple_query_linux_plugin_get_type(), \
                              SimpleQueryLinuxPlugin))

struct _SimpleQueryLinuxPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(SimpleQueryLinuxPlugin, simple_query_linux_plugin, g_object_get_type())

namespace simple_query_linux {

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

struct EdsSource {
  std::string uid;
  std::string display_name;
  bool is_address_book = false;
  bool is_calendar = false;
};

struct EdsSourcesResult {
  std::vector<EdsSource> sources;
  std::optional<std::string> error;
};

struct DomainRowsResult {
  std::vector<EncodableMap> rows;
  std::optional<std::string> error;
};

EdsSourcesResult QueryEdsSources() {
  EdsSourcesResult result;
  GError* error = nullptr;

  GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    result.error = std::string("simple_query: EDS service unavailable - ") +
                   (error != nullptr ? error->message : "session bus is not available");
    if (error != nullptr) {
      g_error_free(error);
    }
    return result;
  }

  GVariant* reply = g_dbus_connection_call_sync(
      connection, "org.gnome.evolution.dataserver.Sources5",
      "/org/gnome/evolution/dataserver/SourceManager",
      "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
      G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

  if (reply == nullptr) {
    result.error = std::string("simple_query: EDS service unavailable - ") +
                   (error != nullptr ? error->message : "GetManagedObjects failed");
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(connection);
    return result;
  }

  GVariantIter* object_iter = nullptr;
  g_variant_get(reply, "(a{oa{sa{sv}}})", &object_iter);
  const gchar* object_path = nullptr;
  GVariant* interfaces = nullptr;

  while (g_variant_iter_next(object_iter, "{&o@a{sa{sv}}}", &object_path, &interfaces)) {
    bool has_source = false;
    bool has_address_book = false;
    bool has_calendar = false;
    std::string uid;
    std::string display_name;

    GVariantIter iface_iter;
    g_variant_iter_init(&iface_iter, interfaces);
    const gchar* interface_name = nullptr;
    GVariant* properties = nullptr;
    while (g_variant_iter_next(&iface_iter, "{&s@a{sv}}", &interface_name, &properties)) {
      const std::string iface = interface_name;
      if (iface == "org.gnome.evolution.dataserver.Source") {
        has_source = true;
        const gchar* value = nullptr;
        if (g_variant_lookup(properties, "Uid", "&s", &value) && value != nullptr) {
          uid = value;
        }
        value = nullptr;
        if (g_variant_lookup(properties, "DisplayName", "&s", &value) && value != nullptr) {
          display_name = value;
        }
      } else if (iface == "org.gnome.evolution.dataserver.Source.AddressBook") {
        has_address_book = true;
      } else if (iface == "org.gnome.evolution.dataserver.Source.Calendar") {
        has_calendar = true;
      }
      g_variant_unref(properties);
    }

    if (has_source && (!uid.empty() || !display_name.empty())) {
      EdsSource source;
      source.uid = uid.empty() ? object_path : uid;
      source.display_name = display_name.empty() ? source.uid : display_name;
      source.is_address_book = has_address_book;
      source.is_calendar = has_calendar;
      result.sources.push_back(std::move(source));
    }

    g_variant_unref(interfaces);
  }

  g_variant_iter_free(object_iter);
  g_variant_unref(reply);
  g_object_unref(connection);
  return result;
}

DomainRowsResult ListContactRecords() {
  DomainRowsResult result;

#ifdef HAS_LIBEBOOK
  GError* error = nullptr;
  ESourceRegistry* registry = e_source_registry_new_sync(nullptr, &error);
  if (registry == nullptr) {
    result.error = std::string("simple_query: EDS registry unavailable - ") +
                   (error ? error->message : "unknown");
    if (error) g_error_free(error);
    return result;
  }

  GList* sources = e_source_registry_list_sources(registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  for (GList* l = sources; l != nullptr; l = l->next) {
    ESource* source = E_SOURCE(l->data);
    EBookClient* client = (EBookClient*)e_book_client_connect_sync(source, 30, nullptr, &error);
    if (client == nullptr) {
      if (error) g_error_free(error);
      error = nullptr;
      continue;
    }

    GSList* contacts = nullptr;
    // Empty query string returns all contacts.
    if (e_book_client_get_contacts_sync(client, "", &contacts, nullptr, &error)) {
      for (GSList* c = contacts; c != nullptr; c = c->next) {
        EContact* contact = E_CONTACT(c->data);
        EncodableMap row;

        const gchar* uid = (const gchar*)e_contact_get_const(contact, E_CONTACT_UID);
        const gchar* full_name = (const gchar*)e_contact_get_const(contact, E_CONTACT_FULL_NAME);

        row[EncodableValue("id")] = EncodableValue(uid ? std::string(uid) : "");
        row[EncodableValue("displayName")] = EncodableValue(full_name ? std::string(full_name) : "");

        // Phones.
        EncodableList phones;
        GList* phone_attrs = e_contact_get_attributes(contact, E_CONTACT_TEL);
        for (GList* p = phone_attrs; p != nullptr; p = p->next) {
          EVCardAttribute* attr = (EVCardAttribute*)p->data;
          gchar* value = e_vcard_attribute_get_value(attr);
          if (value && *value) {
            phones.push_back(EncodableValue(std::string(value)));
          }
          g_free(value);
        }
        g_list_free_full(phone_attrs, (GDestroyNotify)e_vcard_attribute_free);
        row[EncodableValue("phones")] = EncodableValue(phones);

        // Emails.
        EncodableList emails;
        GList* email_attrs = e_contact_get_attributes(contact, E_CONTACT_EMAIL);
        for (GList* e = email_attrs; e != nullptr; e = e->next) {
          EVCardAttribute* attr = (EVCardAttribute*)e->data;
          gchar* value = e_vcard_attribute_get_value(attr);
          if (value && *value) {
            emails.push_back(EncodableValue(std::string(value)));
          }
          g_free(value);
        }
        g_list_free_full(email_attrs, (GDestroyNotify)e_vcard_attribute_free);
        row[EncodableValue("emails")] = EncodableValue(emails);

        const gchar* org = (const gchar*)e_contact_get_const(contact, E_CONTACT_ORG);
        row[EncodableValue("organization")] = org ? EncodableValue(std::string(org)) : EncodableValue();

        // EDS does not expose per-contact modification timestamps easily.
        row[EncodableValue("updatedAt")] = EncodableValue();

        result.rows.push_back(std::move(row));
        g_object_unref(contact);
      }
      g_slist_free(contacts);
    } else {
      if (error) g_error_free(error);
      error = nullptr;
    }

    g_object_unref(client);
  }

  g_list_free_full(sources, g_object_unref);
  g_object_unref(registry);
#else
  // Fallback: list address book sources as placeholder records.
  const auto sources = QueryEdsSources();
  if (sources.error.has_value()) {
    result.error = *sources.error;
    return result;
  }
  for (const auto& source : sources.sources) {
    if (!source.is_address_book) continue;
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(source.uid);
    row[EncodableValue("displayName")] = EncodableValue(source.display_name);
    row[EncodableValue("phones")] = EncodableValue(EncodableList{});
    row[EncodableValue("emails")] = EncodableValue(EncodableList{});
    row[EncodableValue("organization")] = EncodableValue("EDS");
    row[EncodableValue("updatedAt")] = EncodableValue(std::to_string(0));
    result.rows.push_back(std::move(row));
  }
#endif

  return result;
}

DomainRowsResult ListCalendarRecords() {
  DomainRowsResult result;

#ifdef HAS_LIBECAL
  GError* error = nullptr;
  ESourceRegistry* registry = e_source_registry_new_sync(nullptr, &error);
  if (registry == nullptr) {
    result.error = std::string("simple_query: EDS registry unavailable - ") +
                   (error ? error->message : "unknown");
    if (error) g_error_free(error);
    return result;
  }

  GList* sources = e_source_registry_list_sources(registry, E_SOURCE_EXTENSION_CALENDAR);

  // Query range: 1 year back to 1 year forward.
  time_t now = time(nullptr);
  time_t start_time = now - 31536000;
  time_t end_time = now + 31536000;
  ICalTime* ical_start = i_cal_time_new_from_timet_with_zone(start_time, 0, nullptr);
  ICalTime* ical_end = i_cal_time_new_from_timet_with_zone(end_time, 0, nullptr);
  gchar* iso_start = i_cal_time_as_ical_string(ical_start);
  gchar* iso_end = i_cal_time_as_ical_string(ical_end);
  gchar* sexp = g_strdup_printf(
      "(occur-in-time-range? (make-time \"%s\") (make-time \"%s\"))",
      iso_start, iso_end);

  for (GList* l = sources; l != nullptr; l = l->next) {
    ESource* source = E_SOURCE(l->data);
    ECalClient* client = (ECalClient*)e_cal_client_connect_sync(
        source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, nullptr, &error);
    if (client == nullptr) {
      if (error) g_error_free(error);
      error = nullptr;
      continue;
    }

    const gchar* cal_uid = e_source_get_uid(source);
    GSList* ical_comps = nullptr;
    if (e_cal_client_get_object_list_sync(client, sexp, &ical_comps, nullptr, &error)) {
      for (GSList* c = ical_comps; c != nullptr; c = c->next) {
        ICalComponent* comp = I_CAL_COMPONENT(c->data);
        if (i_cal_component_isa(comp) != I_CAL_VEVENT_COMPONENT) {
          g_object_unref(comp);
          continue;
        }

        EncodableMap row;

        const gchar* uid = i_cal_component_get_uid(comp);
        const gchar* summary = i_cal_component_get_summary(comp);
        ICalTime* dtstart = i_cal_component_get_dtstart(comp);
        ICalTime* dtend = i_cal_component_get_dtend(comp);

        row[EncodableValue("id")] = EncodableValue(uid ? std::string(uid) : "");
        row[EncodableValue("title")] = EncodableValue(summary ? std::string(summary) : "");

        if (dtstart) {
          gchar* start_str = i_cal_time_as_ical_string(dtstart);
          row[EncodableValue("startAt")] = EncodableValue(start_str ? std::string(start_str) : "");
          g_free(start_str);

          // isAllDay: date-only times are all-day events.
          row[EncodableValue("isAllDay")] = EncodableValue(i_cal_time_is_date(dtstart) != 0);
          g_object_unref(dtstart);
        } else {
          row[EncodableValue("startAt")] = EncodableValue("");
          row[EncodableValue("isAllDay")] = EncodableValue(false);
        }

        if (dtend) {
          gchar* end_str = i_cal_time_as_ical_string(dtend);
          row[EncodableValue("endAt")] = EncodableValue(end_str ? std::string(end_str) : "");
          g_free(end_str);
          g_object_unref(dtend);
        } else {
          row[EncodableValue("endAt")] = EncodableValue("");
        }

        row[EncodableValue("calendarId")] = EncodableValue(cal_uid ? std::string(cal_uid) : "");

        ICalTime* last_modified = i_cal_component_get_recurrenceid(comp);
        if (last_modified) {
          gchar* mod_str = i_cal_time_as_ical_string(last_modified);
          row[EncodableValue("updatedAt")] = mod_str ? EncodableValue(std::string(mod_str)) : EncodableValue();
          g_free(mod_str);
          g_object_unref(last_modified);
        } else {
          row[EncodableValue("updatedAt")] = EncodableValue();
        }

        result.rows.push_back(std::move(row));
        g_object_unref(comp);
      }
      g_slist_free(ical_comps);
    } else {
      if (error) g_error_free(error);
      error = nullptr;
    }

    g_object_unref(client);
  }

  g_free(sexp);
  g_free(iso_start);
  g_free(iso_end);
  g_object_unref(ical_start);
  g_object_unref(ical_end);
  g_list_free_full(sources, g_object_unref);
  g_object_unref(registry);
#else
  // Fallback: list calendar sources as placeholder records.
  const auto sources = QueryEdsSources();
  if (sources.error.has_value()) {
    result.error = *sources.error;
    return result;
  }
  for (const auto& source : sources.sources) {
    if (!source.is_calendar) continue;
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(source.uid);
    row[EncodableValue("title")] = EncodableValue(source.display_name);
    row[EncodableValue("startAt")] = EncodableValue(std::to_string(0));
    row[EncodableValue("endAt")] = EncodableValue(std::to_string(0));
    row[EncodableValue("isAllDay")] = EncodableValue(false);
    row[EncodableValue("calendarId")] = EncodableValue(source.uid);
    row[EncodableValue("updatedAt")] = EncodableValue(std::to_string(0));
    result.rows.push_back(std::move(row));
  }
#endif

  return result;
}

EdsSourcesResult ListEdsDomainSources(bool address_books) {
  EdsSourcesResult result;
  const auto sources = QueryEdsSources();
  if (sources.error.has_value()) {
    result.error = sources.error;
    return result;
  }

  for (const auto& source : sources.sources) {
    if (address_books && source.is_address_book) {
      result.sources.push_back(source);
    }
    if (!address_books && source.is_calendar) {
      result.sources.push_back(source);
    }
  }
  return result;
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
  EncodableList ids;
  for (const auto& [id, modified] : lhs) {
    auto it = rhs.find(id);
    if (it == rhs.end() || it->second != modified) {
      ids.push_back(EncodableValue(id));
    }
  }
  for (const auto& [id, _] : rhs) {
    if (lhs.find(id) == lhs.end()) {
      ids.push_back(EncodableValue(id));
    }
  }
  return ids;
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
    capabilities.push_back(EncodableValue(Capability("contacts",
                                                     !contacts_probe.error.has_value(),
                                                     false,
                                                     !contacts_probe.error.has_value(),
                                                     false,
                                                     contacts_probe.error)));
    capabilities.push_back(EncodableValue(Capability("media", true, true, true, true)));
    capabilities.push_back(EncodableValue(Capability("files", true, true, true, true)));
    capabilities.push_back(EncodableValue(Capability("calendar",
                                                     !calendar_probe.error.has_value(),
                                                     false,
                                                     !calendar_probe.error.has_value(),
                                                     false,
                                                     calendar_probe.error)));
    capabilities.push_back(EncodableValue(
        Capability("messages", false, false, false, false,
                   "simple_query: messages is not supported on Linux")));
    capabilities.push_back(EncodableValue(
        Capability("calls", false, false, false, false,
                   "simple_query: calls is not supported on Linux")));
    capabilities.push_back(
        EncodableValue(Capability("platformSpecific", true, false, false, false)));

    EncodableMap extensions;
    extensions[EncodableValue("linux.eds")] = EncodableValue(true);
    extensions[EncodableValue("linux.tracker")] = EncodableValue(true);
    extensions[EncodableValue("linux.xdg")] = EncodableValue(true);

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
                              " on Linux host");
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
                              " on Linux host");
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
                              " on Linux host");
    }

    int64_t interval_ms = AsInt(FindValue(request, "pollingIntervalMs")).value_or(1000);
    interval_ms = std::max<int64_t>(250, interval_ms);

    const std::string observer_id =
        std::string("linux_observer_") + std::to_string(++observer_counter_);
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
        event[EncodableValue("source")] = EncodableValue("linux-host");
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
                              " on Linux host");
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

    const auto handle = std::string("linux_handle_") + std::to_string(++handle_counter_);
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
    metadata[EncodableValue("source")] = EncodableValue("linux-host");
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
    if (namespaze == "linux.eds" && method == "listAddressBooks") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: linux.eds.listAddressBooks does not accept arguments");
      }
      const auto books_result = ListEdsDomainSources(true);
      if (books_result.error.has_value()) {
        return FlutterError("unavailable", *books_result.error);
      }
      EncodableList books;
      for (const auto& source : books_result.sources) {
        EncodableMap row;
        row[EncodableValue("id")] = EncodableValue(source.uid);
        row[EncodableValue("name")] = EncodableValue(source.display_name);
        books.push_back(EncodableValue(row));
      }

      EncodableMap response;
      response[EncodableValue("addressBooks")] = EncodableValue(books);
      return response;
    }

    if (namespaze == "linux.eds" && method == "listCalendars") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: linux.eds.listCalendars does not accept arguments");
      }
      const auto calendars_result = ListEdsDomainSources(false);
      if (calendars_result.error.has_value()) {
        return FlutterError("unavailable", *calendars_result.error);
      }
      EncodableList calendars;
      for (const auto& source : calendars_result.sources) {
        EncodableMap row;
        row[EncodableValue("id")] = EncodableValue(source.uid);
        row[EncodableValue("title")] = EncodableValue(source.display_name);
        calendars.push_back(EncodableValue(row));
      }
      EncodableMap response;
      response[EncodableValue("calendars")] = EncodableValue(calendars);
      return response;
    }

    if (namespaze == "linux.tracker" && method == "listIndexScopes") {
      int64_t limit = -1;
      if (args != nullptr) {
        if (const auto* limit_value = FindValue(*args, "limit")) {
          const auto parsed = AsInt(limit_value);
          if (!parsed.has_value()) {
            return FlutterError(
                "invalid-query",
                "simple_query: linux.tracker.listIndexScopes expects limit as int");
          }
          limit = *parsed;
        }
      }

      EncodableList scopes;
      scopes.push_back(EncodableValue("home"));
      scopes.push_back(EncodableValue("media"));
      if (limit >= 0) {
        EncodableList limited;
        for (int64_t i = 0; i < limit && i < static_cast<int64_t>(scopes.size()); i++) {
          limited.push_back(scopes[static_cast<size_t>(i)]);
        }
        scopes = limited;
      }
      EncodableMap response;
      response[EncodableValue("scopes")] = EncodableValue(scopes);
      return response;
    }

    if (namespaze == "linux.tracker" && method == "listGraphNames") {
      if (args != nullptr && !args->empty()) {
        return FlutterError(
            "invalid-query",
            "simple_query: linux.tracker.listGraphNames does not accept arguments");
      }
      EncodableList graphs;
      graphs.push_back(EncodableValue("tracker:Documents"));
      graphs.push_back(EncodableValue("tracker:Pictures"));
      graphs.push_back(EncodableValue("tracker:Audio"));
      graphs.push_back(EncodableValue("tracker:Video"));
      EncodableMap response;
      response[EncodableValue("graphs")] = EncodableValue(graphs);
      return response;
    }

    if (namespaze == "linux.xdg" && method == "listIndexScopes") {
      bool include_temp = true;
      if (args != nullptr) {
        if (const auto* include_temp_value = FindValue(*args, "includeTemp")) {
          if (const auto* include_temp_bool = std::get_if<bool>(include_temp_value)) {
            include_temp = *include_temp_bool;
          } else {
            return FlutterError(
                "invalid-query",
                "simple_query: linux.xdg.listIndexScopes expects includeTemp as bool");
          }
        }
      }
      EncodableList scopes;
      scopes.push_back(EncodableValue(std::filesystem::current_path().string()));
      if (include_temp) {
        scopes.push_back(EncodableValue(std::filesystem::temp_directory_path().string()));
      }
      EncodableMap response;
      response[EncodableValue("scopes")] = EncodableValue(scopes);
      return response;
    }

    return FlutterError("not-supported",
                        "simple_query: " + namespaze + "." + method +
                            " is not supported on Linux host");
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

}  // namespace simple_query_linux

static void simple_query_linux_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(simple_query_linux_plugin_parent_class)->dispose(object);
}

static void simple_query_linux_plugin_class_init(
    SimpleQueryLinuxPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = simple_query_linux_plugin_dispose;
}

static void simple_query_linux_plugin_init(SimpleQueryLinuxPlugin* self) {}

void simple_query_linux_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  auto* plugin = SIMPLE_QUERY_LINUX_PLUGIN(
      g_object_new(simple_query_linux_plugin_get_type(), nullptr));

  static std::unique_ptr<simple_query_linux::NativeQueryHostApiImpl> host_api;
  if (!host_api) {
    host_api = std::make_unique<simple_query_linux::NativeQueryHostApiImpl>(
        fl_plugin_registrar_get_messenger(registrar));
  }
  simple_query_linux::NativeQueryHostApi::SetUp(
      fl_plugin_registrar_get_messenger(registrar), host_api.get());

  g_object_unref(plugin);
}
