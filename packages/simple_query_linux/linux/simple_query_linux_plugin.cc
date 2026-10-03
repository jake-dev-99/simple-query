#include "include/simple_query_linux/simple_query_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

#define SIMPLE_QUERY_LINUX_PLUGIN(obj)                                  \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), simple_query_linux_plugin_get_type(), \
                              SimpleQueryLinuxPlugin))

namespace simple_query_linux {

namespace {

struct FlValueDeleter {
  void operator()(FlValue* value) const {
    if (value != nullptr) {
      fl_value_unref(value);
    }
  }
};

using ValuePtr = std::unique_ptr<FlValue, FlValueDeleter>;
using Rows = std::vector<ValuePtr>;

ValuePtr Value(FlValue* value) { return ValuePtr(value); }

void MapSet(FlValue* map, const char* key, FlValue* value) {
  fl_value_set_string_take(map, key, value);
}

void MapSetString(FlValue* map, const char* key, const std::string& value) {
  MapSet(map, key, fl_value_new_string(value.c_str()));
}

void MapSetBool(FlValue* map, const char* key, bool value) {
  MapSet(map, key, fl_value_new_bool(value));
}

void MapSetInt(FlValue* map, const char* key, int64_t value) {
  MapSet(map, key, fl_value_new_int(value));
}

FlValue* FindValue(FlValue* map, const std::string& key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }
  return fl_value_lookup_string(map, key.c_str());
}

std::optional<std::string> AsString(FlValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_STRING:
      return std::string(fl_value_get_string(value));
    case FL_VALUE_TYPE_INT:
      return std::to_string(fl_value_get_int(value));
    case FL_VALUE_TYPE_FLOAT:
      return std::to_string(fl_value_get_float(value));
    case FL_VALUE_TYPE_BOOL:
      return fl_value_get_bool(value) ? "true" : "false";
    default:
      return std::nullopt;
  }
}

std::string StringOr(FlValue* map, const std::string& key,
                     const std::string& fallback) {
  return AsString(FindValue(map, key)).value_or(fallback);
}

std::optional<int64_t> AsInt(FlValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_INT:
      return fl_value_get_int(value);
    case FL_VALUE_TYPE_FLOAT:
      return static_cast<int64_t>(fl_value_get_float(value));
    default:
      return std::nullopt;
  }
}

bool BoolOr(FlValue* map, const std::string& key, bool fallback) {
  FlValue* value = FindValue(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
    return fallback;
  }
  return fl_value_get_bool(value);
}

FlValue* AsMap(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_MAP
             ? value
             : nullptr;
}

FlValue* AsList(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_LIST
             ? value
             : nullptr;
}

ValuePtr CloneMap(FlValue* map) {
  auto clone = Value(fl_value_new_map());
  if (AsMap(map) == nullptr) {
    return clone;
  }
  for (size_t index = 0; index < fl_value_get_length(map); index++) {
    FlValue* key = fl_value_get_map_key(map, index);
    if (fl_value_get_type(key) != FL_VALUE_TYPE_STRING) {
      continue;
    }
    MapSet(clone.get(), fl_value_get_string(key),
           fl_value_ref(fl_value_get_map_value(map, index)));
  }
  return clone;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
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
  Rows rows;
  std::optional<std::string> error;
};

EdsSourcesResult QueryEdsSources() {
  EdsSourcesResult result;
  GError* error = nullptr;

  GDBusConnection* connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    result.error = std::string("simple_query: EDS service unavailable - ") +
                   (error != nullptr ? error->message
                                     : "session bus is not available");
    if (error != nullptr) {
      g_error_free(error);
    }
    return result;
  }

  GVariant* reply = g_dbus_connection_call_sync(
      connection, "org.gnome.evolution.dataserver.Sources5",
      "/org/gnome/evolution/dataserver/SourceManager",
      "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
      G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
      &error);

  if (reply == nullptr) {
    result.error = std::string("simple_query: EDS service unavailable - ") +
                   (error != nullptr ? error->message
                                     : "GetManagedObjects failed");
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

  while (g_variant_iter_next(object_iter, "{&o@a{sa{sv}}}", &object_path,
                             &interfaces)) {
    bool has_source = false;
    bool has_address_book = false;
    bool has_calendar = false;
    std::string uid;
    std::string display_name;

    GVariantIter iface_iter;
    g_variant_iter_init(&iface_iter, interfaces);
    const gchar* interface_name = nullptr;
    GVariant* properties = nullptr;
    while (g_variant_iter_next(&iface_iter, "{&s@a{sv}}", &interface_name,
                               &properties)) {
      const std::string iface = interface_name;
      if (iface == "org.gnome.evolution.dataserver.Source") {
        has_source = true;
        const gchar* value = nullptr;
        if (g_variant_lookup(properties, "Uid", "&s", &value) &&
            value != nullptr) {
          uid = value;
        }
        value = nullptr;
        if (g_variant_lookup(properties, "DisplayName", "&s", &value) &&
            value != nullptr) {
          display_name = value;
        }
      } else if (iface ==
                 "org.gnome.evolution.dataserver.Source.AddressBook") {
        has_address_book = true;
      } else if (iface ==
                 "org.gnome.evolution.dataserver.Source.Calendar") {
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

  GList* sources =
      e_source_registry_list_sources(registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  for (GList* link = sources; link != nullptr; link = link->next) {
    ESource* source = E_SOURCE(link->data);
    EBookClient* client = reinterpret_cast<EBookClient*>(
        e_book_client_connect_sync(source, 30, nullptr, &error));
    if (client == nullptr) {
      if (error) g_error_free(error);
      error = nullptr;
      continue;
    }

    GSList* contacts = nullptr;
    if (e_book_client_get_contacts_sync(client, "", &contacts, nullptr,
                                        &error)) {
      for (GSList* contact_link = contacts; contact_link != nullptr;
           contact_link = contact_link->next) {
        EContact* contact = E_CONTACT(contact_link->data);
        auto row = Value(fl_value_new_map());

        const gchar* uid = reinterpret_cast<const gchar*>(
            e_contact_get_const(contact, E_CONTACT_UID));
        const gchar* full_name = reinterpret_cast<const gchar*>(
            e_contact_get_const(contact, E_CONTACT_FULL_NAME));
        MapSetString(row.get(), "id", uid != nullptr ? uid : "");
        MapSetString(row.get(), "displayName",
                     full_name != nullptr ? full_name : "");

        auto phones = Value(fl_value_new_list());
        GList* phone_attrs = e_contact_get_attributes(contact, E_CONTACT_TEL);
        for (GList* phone = phone_attrs; phone != nullptr;
             phone = phone->next) {
          auto* attr = static_cast<EVCardAttribute*>(phone->data);
          gchar* value = e_vcard_attribute_get_value(attr);
          if (value != nullptr && *value != '\0') {
            fl_value_append_take(phones.get(), fl_value_new_string(value));
          }
          g_free(value);
        }
        g_list_free_full(phone_attrs,
                         reinterpret_cast<GDestroyNotify>(
                             e_vcard_attribute_free));
        MapSet(row.get(), "phones", phones.release());

        auto emails = Value(fl_value_new_list());
        GList* email_attrs = e_contact_get_attributes(contact, E_CONTACT_EMAIL);
        for (GList* email = email_attrs; email != nullptr;
             email = email->next) {
          auto* attr = static_cast<EVCardAttribute*>(email->data);
          gchar* value = e_vcard_attribute_get_value(attr);
          if (value != nullptr && *value != '\0') {
            fl_value_append_take(emails.get(), fl_value_new_string(value));
          }
          g_free(value);
        }
        g_list_free_full(email_attrs,
                         reinterpret_cast<GDestroyNotify>(
                             e_vcard_attribute_free));
        MapSet(row.get(), "emails", emails.release());

        const gchar* organization = reinterpret_cast<const gchar*>(
            e_contact_get_const(contact, E_CONTACT_ORG));
        MapSet(row.get(), "organization",
               organization != nullptr ? fl_value_new_string(organization)
                                       : fl_value_new_null());
        MapSet(row.get(), "updatedAt", fl_value_new_null());
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
  const auto sources = QueryEdsSources();
  if (sources.error.has_value()) {
    result.error = *sources.error;
    return result;
  }
  for (const auto& source : sources.sources) {
    if (!source.is_address_book) continue;
    auto row = Value(fl_value_new_map());
    MapSetString(row.get(), "id", source.uid);
    MapSetString(row.get(), "displayName", source.display_name);
    MapSet(row.get(), "phones", fl_value_new_list());
    MapSet(row.get(), "emails", fl_value_new_list());
    MapSetString(row.get(), "organization", "EDS");
    MapSetString(row.get(), "updatedAt", "0");
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

  GList* sources =
      e_source_registry_list_sources(registry, E_SOURCE_EXTENSION_CALENDAR);
  time_t now = time(nullptr);
  ICalTime* ical_start =
      i_cal_time_new_from_timet_with_zone(now - 31536000, 0, nullptr);
  ICalTime* ical_end =
      i_cal_time_new_from_timet_with_zone(now + 31536000, 0, nullptr);
  gchar* iso_start = i_cal_time_as_ical_string(ical_start);
  gchar* iso_end = i_cal_time_as_ical_string(ical_end);
  gchar* sexp = g_strdup_printf(
      "(occur-in-time-range? (make-time \"%s\") (make-time \"%s\"))",
      iso_start, iso_end);

  for (GList* link = sources; link != nullptr; link = link->next) {
    ESource* source = E_SOURCE(link->data);
    ECalClient* client = reinterpret_cast<ECalClient*>(
        e_cal_client_connect_sync(source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30,
                                  nullptr, &error));
    if (client == nullptr) {
      if (error) g_error_free(error);
      error = nullptr;
      continue;
    }

    const gchar* calendar_uid = e_source_get_uid(source);
    GSList* components = nullptr;
    if (e_cal_client_get_object_list_sync(client, sexp, &components, nullptr,
                                          &error)) {
      for (GSList* component_link = components; component_link != nullptr;
           component_link = component_link->next) {
        ICalComponent* component = I_CAL_COMPONENT(component_link->data);
        if (i_cal_component_isa(component) != I_CAL_VEVENT_COMPONENT) {
          g_object_unref(component);
          continue;
        }

        auto row = Value(fl_value_new_map());
        const gchar* uid = i_cal_component_get_uid(component);
        const gchar* summary = i_cal_component_get_summary(component);
        ICalTime* start = i_cal_component_get_dtstart(component);
        ICalTime* end = i_cal_component_get_dtend(component);
        MapSetString(row.get(), "id", uid != nullptr ? uid : "");
        MapSetString(row.get(), "title",
                     summary != nullptr ? summary : "");

        if (start != nullptr) {
          gchar* start_string = i_cal_time_as_ical_string(start);
          MapSetString(row.get(), "startAt",
                       start_string != nullptr ? start_string : "");
          MapSetBool(row.get(), "isAllDay",
                     i_cal_time_is_date(start) != 0);
          g_free(start_string);
          g_object_unref(start);
        } else {
          MapSetString(row.get(), "startAt", "");
          MapSetBool(row.get(), "isAllDay", false);
        }

        if (end != nullptr) {
          gchar* end_string = i_cal_time_as_ical_string(end);
          MapSetString(row.get(), "endAt",
                       end_string != nullptr ? end_string : "");
          g_free(end_string);
          g_object_unref(end);
        } else {
          MapSetString(row.get(), "endAt", "");
        }

        MapSetString(row.get(), "calendarId",
                     calendar_uid != nullptr ? calendar_uid : "");
        ICalTime* modified = i_cal_component_get_recurrenceid(component);
        if (modified != nullptr) {
          gchar* modified_string = i_cal_time_as_ical_string(modified);
          MapSet(row.get(), "updatedAt",
                 modified_string != nullptr
                     ? fl_value_new_string(modified_string)
                     : fl_value_new_null());
          g_free(modified_string);
          g_object_unref(modified);
        } else {
          MapSet(row.get(), "updatedAt", fl_value_new_null());
        }

        result.rows.push_back(std::move(row));
        g_object_unref(component);
      }
      g_slist_free(components);
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
  const auto sources = QueryEdsSources();
  if (sources.error.has_value()) {
    result.error = *sources.error;
    return result;
  }
  for (const auto& source : sources.sources) {
    if (!source.is_calendar) continue;
    auto row = Value(fl_value_new_map());
    MapSetString(row.get(), "id", source.uid);
    MapSetString(row.get(), "title", source.display_name);
    MapSetString(row.get(), "startAt", "0");
    MapSetString(row.get(), "endAt", "0");
    MapSetBool(row.get(), "isAllDay", false);
    MapSetString(row.get(), "calendarId", source.uid);
    MapSetString(row.get(), "updatedAt", "0");
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
    if ((address_books && source.is_address_book) ||
        (!address_books && source.is_calendar)) {
      result.sources.push_back(source);
    }
  }
  return result;
}

int64_t ModifiedEpochMs(const std::filesystem::directory_entry& entry) {
  auto ticks = entry.last_write_time().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count();
}

std::string ValueAsString(FlValue* row, const std::string& key) {
  return AsString(FindValue(row, key)).value_or("");
}

ValuePtr Capability(const std::string& domain, bool can_read, bool can_write,
                    bool can_observe, bool can_stream,
                    const std::optional<std::string>& reason = std::nullopt) {
  auto map = Value(fl_value_new_map());
  MapSetString(map.get(), "domain", domain);
  MapSetBool(map.get(), "canRead", can_read);
  MapSetBool(map.get(), "canWrite", can_write);
  MapSetBool(map.get(), "canObserve", can_observe);
  MapSetBool(map.get(), "canStream", can_stream);
  if (reason.has_value()) {
    MapSetString(map.get(), "reason", *reason);
  }
  return map;
}

std::filesystem::path ResolveRootPath(FlValue* request) {
  FlValue* platform_data = AsMap(FindValue(request, "platformData"));
  if (platform_data != nullptr) {
    const auto root = AsString(FindValue(platform_data, "rootPath"));
    if (root.has_value() && !root->empty()) {
      return std::filesystem::path(*root);
    }
  }
  return std::filesystem::current_path();
}

Rows ListRecords(const std::filesystem::path& root, bool media_only) {
  Rows rows;
  std::error_code error;
  if (!std::filesystem::exists(root, error)) {
    return rows;
  }

  for (auto iterator = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error);
       iterator != std::filesystem::recursive_directory_iterator();
       iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }

    const auto& entry = *iterator;
    const auto path = entry.path();
    const bool is_directory = entry.is_directory(error);
    const std::string mime = MimeFromPath(path);

    auto row = Value(fl_value_new_map());
    if (media_only) {
      if (is_directory || !IsMediaMime(mime)) {
        continue;
      }
      MapSetString(row.get(), "id", path.string());
      MapSetString(row.get(), "uriOrPath", path.string());
      MapSetString(row.get(), "mediaType", MediaType(mime));
      MapSetString(row.get(), "mimeType", mime);
      if (entry.is_regular_file(error)) {
        MapSetInt(row.get(), "size",
                  static_cast<int64_t>(entry.file_size(error)));
      }
      const auto modified = ModifiedEpochMs(entry);
      MapSetString(row.get(), "createdAt", std::to_string(modified));
      MapSetString(row.get(), "modifiedAt", std::to_string(modified));
      rows.push_back(std::move(row));
      continue;
    }

    MapSetString(row.get(), "id", path.string());
    MapSetString(row.get(), "path", path.string());
    MapSetString(row.get(), "name", path.filename().string());
    MapSetBool(row.get(), "isDirectory", is_directory);
    if (entry.is_regular_file(error)) {
      MapSetInt(row.get(), "size",
                static_cast<int64_t>(entry.file_size(error)));
    }
    const auto modified = ModifiedEpochMs(entry);
    MapSetString(row.get(), "modifiedAt", std::to_string(modified));
    MapSetString(row.get(), "mimeType", mime);
    MapSetString(row.get(), "type", is_directory ? "directory" : "file");
    MapSetString(row.get(), "extension", path.extension().string());
    MapSetInt(row.get(), "modifiedEpochMs", modified);
    rows.push_back(std::move(row));
  }

  return rows;
}

Rows ApplyFilters(Rows rows, FlValue* filters) {
  if (filters == nullptr || fl_value_get_length(filters) == 0) {
    return rows;
  }

  Rows filtered;
  for (auto& row : rows) {
    bool keep = true;
    for (size_t index = 0; index < fl_value_get_length(filters); index++) {
      FlValue* filter = AsMap(fl_value_get_list_value(filters, index));
      if (filter == nullptr) {
        continue;
      }
      const auto field = AsString(FindValue(filter, "field"));
      const auto operation = AsString(FindValue(filter, "operator"));
      if (!field.has_value() || !operation.has_value()) {
        continue;
      }

      const std::string actual = ValueAsString(row.get(), *field);
      FlValue* expected = FindValue(filter, "value");
      if (*operation == "equals") {
        if (actual != AsString(expected).value_or("")) {
          keep = false;
          break;
        }
      } else if (*operation == "contains") {
        const std::string needle = Lower(AsString(expected).value_or(""));
        if (Lower(actual).find(needle) == std::string::npos) {
          keep = false;
          break;
        }
      } else if (*operation == "inList") {
        FlValue* expected_list = AsList(expected);
        if (expected_list != nullptr) {
          bool matched = false;
          for (size_t item_index = 0;
               item_index < fl_value_get_length(expected_list); item_index++) {
            if (actual ==
                AsString(fl_value_get_list_value(expected_list, item_index))
                    .value_or("")) {
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

void ApplySort(Rows* rows, FlValue* sort) {
  if (sort == nullptr || fl_value_get_length(sort) == 0) {
    return;
  }
  FlValue* first = AsMap(fl_value_get_list_value(sort, 0));
  if (first == nullptr) {
    return;
  }
  const auto field = AsString(FindValue(first, "field"));
  if (!field.has_value()) {
    return;
  }
  const bool ascending =
      AsString(FindValue(first, "direction")).value_or("ascending") !=
      "descending";
  std::sort(rows->begin(), rows->end(),
            [&](const ValuePtr& left, const ValuePtr& right) {
              const auto left_value = ValueAsString(left.get(), *field);
              const auto right_value = ValueAsString(right.get(), *field);
              return ascending ? left_value < right_value
                               : left_value > right_value;
            });
}

ValuePtr ApplyProjection(const Rows& rows, FlValue* projection) {
  auto projected_rows = Value(fl_value_new_list());
  if (projection == nullptr || fl_value_get_length(projection) == 0) {
    for (const auto& row : rows) {
      fl_value_append_take(projected_rows.get(), fl_value_ref(row.get()));
    }
    return projected_rows;
  }

  for (const auto& row : rows) {
    auto projected = Value(fl_value_new_map());
    for (size_t index = 0; index < fl_value_get_length(projection); index++) {
      const auto key =
          AsString(fl_value_get_list_value(projection, index)).value_or("");
      FlValue* value = FindValue(row.get(), key);
      MapSet(projected.get(), key.c_str(),
             value != nullptr ? fl_value_ref(value) : fl_value_new_null());
    }
    fl_value_append_take(projected_rows.get(), projected.release());
  }
  return projected_rows;
}

using Snapshot = std::map<std::string, int64_t>;

Snapshot BuildSnapshotForDomain(FlValue* request, const std::string& domain) {
  Snapshot snapshot;
  Rows rows;
  if (domain == "files" || domain == "media") {
    rows = ListRecords(ResolveRootPath(request), domain == "media");
  } else if (domain == "contacts") {
    rows = ListContactRecords().rows;
  } else if (domain == "calendar") {
    rows = ListCalendarRecords().rows;
  }

  for (const auto& row : rows) {
    const auto id = AsString(FindValue(row.get(), "id"));
    if (!id.has_value()) {
      continue;
    }
    const int64_t modified =
        AsInt(FindValue(row.get(), "modifiedEpochMs"))
            .value_or(AsInt(FindValue(row.get(), "updatedAt"))
                          .value_or(AsInt(FindValue(row.get(), "startAt"))
                                        .value_or(0)));
    snapshot[*id] = modified;
  }
  return snapshot;
}

ValuePtr ChangedIds(const Snapshot& previous, const Snapshot& current) {
  auto ids = Value(fl_value_new_list());
  for (const auto& [id, modified] : previous) {
    const auto found = current.find(id);
    if (found == current.end() || found->second != modified) {
      fl_value_append_take(ids.get(), fl_value_new_string(id.c_str()));
    }
  }
  for (const auto& [id, ignored] : current) {
    if (previous.find(id) == previous.end()) {
      fl_value_append_take(ids.get(), fl_value_new_string(id.c_str()));
    }
  }
  return ids;
}

struct ObserverState {
  std::atomic<bool> active{true};
  std::thread worker;
};

struct NativeError {
  std::string code;
  std::string message;
};

struct ValueResult {
  ValuePtr value;
  std::optional<NativeError> error;
};

ValueResult Success(ValuePtr value) {
  return ValueResult{std::move(value), std::nullopt};
}

ValueResult Failure(const std::string& code, const std::string& message) {
  return ValueResult{ValuePtr(), NativeError{code, message}};
}

struct StringResult {
  std::string value;
  std::optional<NativeError> error;
};

}  // namespace

class NativeQueryHostApiImpl {
 public:
  explicit NativeQueryHostApiImpl(FlBinaryMessenger* messenger)
      : flutter_api_(sqlq_native_query_flutter_api_new(messenger, nullptr)) {}

  ~NativeQueryHostApiImpl() {
    ShutdownObservers();
    g_clear_object(&flutter_api_);
  }

  ValueResult GetCapabilities() {
    const auto contacts_probe = ListContactRecords();
    const auto calendar_probe = ListCalendarRecords();

    auto capabilities = Value(fl_value_new_list());
    fl_value_append_take(
        capabilities.get(),
        Capability("contacts", !contacts_probe.error.has_value(), false,
                   !contacts_probe.error.has_value(), false,
                   contacts_probe.error)
            .release());
    fl_value_append_take(capabilities.get(),
                         Capability("media", true, true, true, true).release());
    fl_value_append_take(capabilities.get(),
                         Capability("files", true, true, true, true).release());
    fl_value_append_take(
        capabilities.get(),
        Capability("calendar", !calendar_probe.error.has_value(), false,
                   !calendar_probe.error.has_value(), false,
                   calendar_probe.error)
            .release());
    fl_value_append_take(
        capabilities.get(),
        Capability("messages", false, false, false, false,
                   "simple_query: messages is not supported on Linux")
            .release());
    fl_value_append_take(
        capabilities.get(),
        Capability("calls", false, false, false, false,
                   "simple_query: calls is not supported on Linux")
            .release());
    fl_value_append_take(
        capabilities.get(),
        Capability("platformSpecific", true, false, false, false).release());

    auto extensions = Value(fl_value_new_map());
    MapSetBool(extensions.get(), "linux.eds", true);
    MapSetBool(extensions.get(), "linux.tracker", true);
    MapSetBool(extensions.get(), "linux.xdg", true);

    auto result = Value(fl_value_new_map());
    MapSet(result.get(), "capabilities", capabilities.release());
    MapSet(result.get(), "platformExtensions", extensions.release());
    return Success(std::move(result));
  }

  ValueResult Query(FlValue* request) {
    const std::string domain =
        StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media" && domain != "contacts" &&
        domain != "calendar") {
      return Failure(
          "not-supported",
          "simple_query: query is not supported for domain " + domain +
              " on Linux host");
    }

    Rows rows;
    if (domain == "files" || domain == "media") {
      rows = ListRecords(ResolveRootPath(request), domain == "media");
    } else if (domain == "contacts") {
      auto contact_rows = ListContactRecords();
      if (contact_rows.error.has_value()) {
        return Failure("unavailable", *contact_rows.error);
      }
      rows = std::move(contact_rows.rows);
    } else {
      auto calendar_rows = ListCalendarRecords();
      if (calendar_rows.error.has_value()) {
        return Failure("unavailable", *calendar_rows.error);
      }
      rows = std::move(calendar_rows.rows);
    }

    rows = ApplyFilters(std::move(rows),
                        AsList(FindValue(request, "filters")));
    ApplySort(&rows, AsList(FindValue(request, "sort")));

    const int64_t total_count = static_cast<int64_t>(rows.size());
    FlValue* page = AsMap(FindValue(request, "page"));
    const int64_t offset = std::max<int64_t>(
        0, page == nullptr
               ? 0
               : AsInt(FindValue(page, "offset")).value_or(0));
    const auto limit = page == nullptr
                           ? std::optional<int64_t>{}
                           : AsInt(FindValue(page, "limit"));
    const size_t start =
        static_cast<size_t>(std::min<int64_t>(offset, rows.size()));
    size_t end = rows.size();
    if (limit.has_value()) {
      end = std::min(
          rows.size(),
          start + static_cast<size_t>(std::max<int64_t>(0, *limit)));
    }

    Rows paged_rows;
    for (size_t index = start; index < end; index++) {
      paged_rows.push_back(std::move(rows[index]));
    }

    auto result = Value(fl_value_new_map());
    MapSet(result.get(), "records",
           ApplyProjection(paged_rows,
                           AsList(FindValue(request, "projection")))
               .release());
    MapSetInt(result.get(), "totalCount", total_count);
    if (end < rows.size()) {
      MapSetInt(result.get(), "nextOffset", static_cast<int64_t>(end));
    }
    return Success(std::move(result));
  }

  ValueResult Mutate(FlValue* request) {
    const std::string domain =
        StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media") {
      return Failure(
          "not-supported",
          "simple_query: mutate is not supported for domain " + domain +
              " on Linux host");
    }

    const std::string type = StringOr(request, "type", "");
    if (type == "insert") {
      FlValue* values = AsMap(FindValue(request, "values"));
      if (values == nullptr) {
        return Failure("invalid-query",
                       "simple_query: insert requires values");
      }
      const auto path = AsString(FindValue(values, "path"));
      if (!path.has_value() || path->empty()) {
        return Failure("invalid-query",
                       "simple_query: insert requires values.path");
      }

      std::error_code error;
      if (BoolOr(values, "isDirectory", false)) {
        std::filesystem::create_directories(*path, error);
      } else {
        const auto file_path = std::filesystem::path(*path);
        std::filesystem::create_directories(file_path.parent_path(), error);
        std::ofstream stream(*path, std::ios::binary);
        if (!stream.is_open()) {
          return Failure("unavailable",
                         "simple_query: could not create output file");
        }
        stream << AsString(FindValue(values, "content")).value_or("");
      }

      auto result = Value(fl_value_new_map());
      MapSetInt(result.get(), "affectedCount", 1);
      MapSetString(result.get(), "insertedId", *path);
      return Success(std::move(result));
    }

    if (type == "delete") {
      auto query_request = Value(fl_value_new_map());
      MapSetString(query_request.get(), "domain", domain);
      FlValue* filters = FindValue(request, "filters");
      MapSet(query_request.get(), "filters",
             filters != nullptr ? fl_value_ref(filters) : fl_value_new_list());
      FlValue* platform_data = FindValue(request, "platformData");
      if (platform_data != nullptr) {
        MapSet(query_request.get(), "platformData",
               fl_value_ref(platform_data));
      }

      auto queried = Query(query_request.get());
      if (queried.error.has_value()) {
        return queried;
      }
      FlValue* records = AsList(FindValue(queried.value.get(), "records"));
      int64_t deleted = 0;
      if (records != nullptr) {
        for (size_t index = 0; index < fl_value_get_length(records); index++) {
          FlValue* record = AsMap(fl_value_get_list_value(records, index));
          const auto path = AsString(FindValue(record, "path"));
          if (!path.has_value() || path->empty()) {
            continue;
          }
          std::error_code error;
          deleted += static_cast<int64_t>(
              std::filesystem::remove_all(*path, error));
        }
      }

      auto result = Value(fl_value_new_map());
      MapSetInt(result.get(), "affectedCount", deleted);
      return Success(std::move(result));
    }

    if (type == "update") {
      FlValue* values = AsMap(FindValue(request, "values"));
      if (values == nullptr) {
        return Failure("invalid-query",
                       "simple_query: update requires values");
      }

      std::set<std::string> target_paths;
      const auto explicit_path = AsString(FindValue(values, "path"));
      const auto explicit_id = AsString(FindValue(values, "id"));
      if (explicit_path.has_value() && !explicit_path->empty()) {
        target_paths.insert(*explicit_path);
      } else if (explicit_id.has_value() && !explicit_id->empty()) {
        target_paths.insert(*explicit_id);
      }

      if (target_paths.empty()) {
        auto query_request = Value(fl_value_new_map());
        MapSetString(query_request.get(), "domain", domain);
        FlValue* filters = FindValue(request, "filters");
        MapSet(query_request.get(), "filters",
               filters != nullptr ? fl_value_ref(filters)
                                  : fl_value_new_list());
        FlValue* platform_data = FindValue(request, "platformData");
        if (platform_data != nullptr) {
          MapSet(query_request.get(), "platformData",
                 fl_value_ref(platform_data));
        }
        auto queried = Query(query_request.get());
        if (queried.error.has_value()) {
          return queried;
        }
        FlValue* records = AsList(FindValue(queried.value.get(), "records"));
        if (records != nullptr) {
          for (size_t index = 0; index < fl_value_get_length(records);
               index++) {
            FlValue* record = AsMap(fl_value_get_list_value(records, index));
            const auto path = AsString(FindValue(record, "path"));
            if (path.has_value() && !path->empty()) {
              target_paths.insert(*path);
            }
          }
        }
      }

      int64_t updated = 0;
      for (const auto& original_path : target_paths) {
        std::error_code error;
        std::filesystem::path effective_path(original_path);
        if (!std::filesystem::exists(effective_path, error)) {
          continue;
        }

        bool changed = false;
        const auto new_path = AsString(FindValue(values, "newPath"));
        if (new_path.has_value() && !new_path->empty() &&
            *new_path != original_path) {
          const std::filesystem::path next_path(*new_path);
          std::filesystem::create_directories(next_path.parent_path(), error);
          error.clear();
          std::filesystem::rename(effective_path, next_path, error);
          if (!error) {
            effective_path = next_path;
            changed = true;
          }
        }

        if (!std::filesystem::is_directory(effective_path, error)) {
          FlValue* bytes = FindValue(values, "bytes");
          if (bytes != nullptr) {
            if (fl_value_get_type(bytes) != FL_VALUE_TYPE_UINT8_LIST) {
              return Failure(
                  "invalid-query",
                  "simple_query: update expects values.bytes as Uint8List "
                  "when provided");
            }
            std::ofstream stream(effective_path.string(),
                                 std::ios::binary | std::ios::trunc);
            if (!stream.is_open()) {
              return Failure("unavailable",
                             "simple_query: could not open file for update");
            }
            stream.write(
                reinterpret_cast<const char*>(fl_value_get_uint8_list(bytes)),
                static_cast<std::streamsize>(fl_value_get_length(bytes)));
            changed = true;
          } else if (FlValue* content = FindValue(values, "content");
                     content != nullptr) {
            std::ofstream stream(effective_path.string(),
                                 std::ios::binary | std::ios::trunc);
            if (!stream.is_open()) {
              return Failure("unavailable",
                             "simple_query: could not open file for update");
            }
            stream << AsString(content).value_or("");
            changed = true;
          }
        }

        if (changed) {
          updated += 1;
        }
      }

      auto result = Value(fl_value_new_map());
      MapSetInt(result.get(), "affectedCount", updated);
      return Success(std::move(result));
    }

    return Failure("invalid-query",
                   "simple_query: unknown mutation type " + type);
  }

  ValueResult Batch(FlValue* request) {
    FlValue* operations = AsList(FindValue(request, "operations"));
    if (operations == nullptr) {
      return Failure("invalid-query",
                     "simple_query: batch requires operations");
    }

    auto results = Value(fl_value_new_list());
    for (size_t index = 0; index < fl_value_get_length(operations); index++) {
      FlValue* operation = AsMap(fl_value_get_list_value(operations, index));
      if (operation == nullptr) {
        return Failure("invalid-query",
                       "simple_query: batch operation must be a map");
      }
      auto merged = CloneMap(operation);
      if (FindValue(operation, "platformData") == nullptr &&
          FindValue(request, "platformData") != nullptr) {
        MapSet(merged.get(), "platformData",
               fl_value_ref(FindValue(request, "platformData")));
      }

      auto result = Mutate(merged.get());
      if (result.error.has_value()) {
        return result;
      }
      fl_value_append_take(results.get(), result.value.release());
    }

    auto response = Value(fl_value_new_map());
    MapSet(response.get(), "results", results.release());
    return Success(std::move(response));
  }

  StringResult ObserveStart(FlValue* request) {
    const std::string domain =
        StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media" && domain != "contacts" &&
        domain != "calendar") {
      return StringResult{
          "", NativeError{"not-supported",
                          "simple_query: observe is not supported for domain " +
                              domain + " on Linux host"}};
    }

    int64_t interval_ms =
        AsInt(FindValue(request, "pollingIntervalMs")).value_or(1000);
    interval_ms = std::max<int64_t>(250, interval_ms);
    const std::string observer_id =
        std::string("linux_observer_") +
        std::to_string(++observer_counter_);
    auto state = std::make_unique<ObserverState>();
    auto* state_ptr = state.get();
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      observers_[observer_id] = std::move(state);
    }

    auto request_copy = Value(fl_value_ref(request));
    state_ptr->worker = std::thread(
        [this, observer_id, domain, request = std::move(request_copy),
         interval_ms, state_ptr]() {
          Snapshot previous = BuildSnapshotForDomain(request.get(), domain);
          while (state_ptr->active.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(interval_ms));
            if (!state_ptr->active.load()) {
              break;
            }
            Snapshot current = BuildSnapshotForDomain(request.get(), domain);
            if (current == previous) {
              continue;
            }

            auto event = Value(fl_value_new_map());
            MapSetString(event.get(), "domain", domain);
            MapSetString(event.get(), "changeType", "unknown");
            MapSetString(
                event.get(), "timestamp",
                std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()));
            MapSet(event.get(), "ids",
                   ChangedIds(previous, current).release());
            MapSetString(event.get(), "source", "linux-host");
            previous = std::move(current);
            sqlq_native_query_flutter_api_on_observe_event(
                flutter_api_, observer_id.c_str(), event.get(), nullptr,
                nullptr, nullptr);
          }
        });
    return StringResult{observer_id, std::nullopt};
  }

  std::optional<NativeError> ObserveStop(const std::string& observer_id) {
    std::unique_ptr<ObserverState> state;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      const auto found = observers_.find(observer_id);
      if (found == observers_.end()) {
        return std::nullopt;
      }
      state = std::move(found->second);
      observers_.erase(found);
    }
    state->active.store(false);
    if (state->worker.joinable()) {
      state->worker.join();
    }
    return std::nullopt;
  }

  ValueResult OpenBinary(FlValue* request) {
    const std::string domain =
        StringOr(request, "domain", "platformSpecific");
    if (domain != "files" && domain != "media") {
      return Failure(
          "not-supported",
          "simple_query: openBinary is not supported for domain " + domain +
              " on Linux host");
    }

    std::optional<std::string> path;
    FlValue* platform_data = AsMap(FindValue(request, "platformData"));
    if (platform_data != nullptr) {
      path = AsString(FindValue(platform_data, "path"));
    }
    if (!path.has_value()) {
      path = AsString(FindValue(request, "recordId"));
    }
    if (!path.has_value() || path->empty()) {
      return Failure(
          "invalid-query",
          "simple_query: openBinary requires recordId or platformData.path");
    }

    std::error_code error;
    if (!std::filesystem::exists(*path, error)) {
      return Failure("unavailable",
                     "simple_query: binary resource was not found");
    }

    const auto handle =
        std::string("linux_handle_") + std::to_string(++handle_counter_);
    open_handles_[handle] = *path;
    auto result = Value(fl_value_new_map());
    MapSetString(result.get(), "handleId", handle);
    MapSetString(result.get(), "localPath", *path);
    MapSetString(result.get(), "mimeType", MimeFromPath(*path));
    if (std::filesystem::is_regular_file(*path, error)) {
      MapSetInt(result.get(), "size",
                static_cast<int64_t>(std::filesystem::file_size(*path, error)));
    }
    auto metadata = Value(fl_value_new_map());
    MapSetString(metadata.get(), "source", "linux-host");
    MapSet(result.get(), "metadata", metadata.release());
    return Success(std::move(result));
  }

  std::optional<NativeError> CloseBinary(const std::string& handle_id) {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  ValueResult CallExtension(const std::string& name_space,
                            const std::string& method, FlValue* args) {
    args = AsMap(args);
    if (name_space == "linux.eds" && method == "listAddressBooks") {
      if (args != nullptr && fl_value_get_length(args) != 0) {
        return Failure(
            "invalid-query",
            "simple_query: linux.eds.listAddressBooks does not accept "
            "arguments");
      }
      const auto books_result = ListEdsDomainSources(true);
      if (books_result.error.has_value()) {
        return Failure("unavailable", *books_result.error);
      }
      auto books = Value(fl_value_new_list());
      for (const auto& source : books_result.sources) {
        auto row = Value(fl_value_new_map());
        MapSetString(row.get(), "id", source.uid);
        MapSetString(row.get(), "name", source.display_name);
        fl_value_append_take(books.get(), row.release());
      }
      auto response = Value(fl_value_new_map());
      MapSet(response.get(), "addressBooks", books.release());
      return Success(std::move(response));
    }

    if (name_space == "linux.eds" && method == "listCalendars") {
      if (args != nullptr && fl_value_get_length(args) != 0) {
        return Failure(
            "invalid-query",
            "simple_query: linux.eds.listCalendars does not accept "
            "arguments");
      }
      const auto calendars_result = ListEdsDomainSources(false);
      if (calendars_result.error.has_value()) {
        return Failure("unavailable", *calendars_result.error);
      }
      auto calendars = Value(fl_value_new_list());
      for (const auto& source : calendars_result.sources) {
        auto row = Value(fl_value_new_map());
        MapSetString(row.get(), "id", source.uid);
        MapSetString(row.get(), "title", source.display_name);
        fl_value_append_take(calendars.get(), row.release());
      }
      auto response = Value(fl_value_new_map());
      MapSet(response.get(), "calendars", calendars.release());
      return Success(std::move(response));
    }

    if (name_space == "linux.tracker" && method == "listIndexScopes") {
      int64_t limit = -1;
      if (args != nullptr) {
        if (FlValue* limit_value = FindValue(args, "limit");
            limit_value != nullptr) {
          const auto parsed = AsInt(limit_value);
          if (!parsed.has_value()) {
            return Failure(
                "invalid-query",
                "simple_query: linux.tracker.listIndexScopes expects limit "
                "as int");
          }
          limit = *parsed;
        }
      }

      auto scopes = Value(fl_value_new_list());
      fl_value_append_take(scopes.get(), fl_value_new_string("home"));
      fl_value_append_take(scopes.get(), fl_value_new_string("media"));
      if (limit >= 0 && limit < 2) {
        auto limited = Value(fl_value_new_list());
        for (int64_t index = 0; index < limit; index++) {
          fl_value_append_take(
              limited.get(),
              fl_value_ref(fl_value_get_list_value(
                  scopes.get(), static_cast<size_t>(index))));
        }
        scopes = std::move(limited);
      }
      auto response = Value(fl_value_new_map());
      MapSet(response.get(), "scopes", scopes.release());
      return Success(std::move(response));
    }

    if (name_space == "linux.tracker" && method == "listGraphNames") {
      if (args != nullptr && fl_value_get_length(args) != 0) {
        return Failure(
            "invalid-query",
            "simple_query: linux.tracker.listGraphNames does not accept "
            "arguments");
      }
      auto graphs = Value(fl_value_new_list());
      for (const char* graph : {"tracker:Documents", "tracker:Pictures",
                                "tracker:Audio", "tracker:Video"}) {
        fl_value_append_take(graphs.get(), fl_value_new_string(graph));
      }
      auto response = Value(fl_value_new_map());
      MapSet(response.get(), "graphs", graphs.release());
      return Success(std::move(response));
    }

    if (name_space == "linux.xdg" && method == "listIndexScopes") {
      bool include_temp = true;
      if (args != nullptr) {
        if (FlValue* include_temp_value = FindValue(args, "includeTemp");
            include_temp_value != nullptr) {
          if (fl_value_get_type(include_temp_value) != FL_VALUE_TYPE_BOOL) {
            return Failure(
                "invalid-query",
                "simple_query: linux.xdg.listIndexScopes expects "
                "includeTemp as bool");
          }
          include_temp = fl_value_get_bool(include_temp_value);
        }
      }
      auto scopes = Value(fl_value_new_list());
      fl_value_append_take(
          scopes.get(), fl_value_new_string(
                            std::filesystem::current_path().string().c_str()));
      if (include_temp) {
        fl_value_append_take(
            scopes.get(),
            fl_value_new_string(
                std::filesystem::temp_directory_path().string().c_str()));
      }
      auto response = Value(fl_value_new_map());
      MapSet(response.get(), "scopes", scopes.release());
      return Success(std::move(response));
    }

    return Failure("not-supported", "simple_query: " + name_space + "." +
                                        method +
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

  SqlqNativeQueryFlutterApi* flutter_api_;
  int64_t handle_counter_ = 0;
  int64_t observer_counter_ = 0;
  std::map<std::string, std::string> open_handles_;
  std::map<std::string, std::unique_ptr<ObserverState>> observers_;
  std::mutex observers_mutex_;
};

}  // namespace simple_query_linux

struct _SimpleQueryLinuxPlugin {
  GObject parent_instance;
  FlPluginRegistrar* registrar;
  simple_query_linux::NativeQueryHostApiImpl* host_api;
};

G_DEFINE_TYPE(SimpleQueryLinuxPlugin, simple_query_linux_plugin, G_TYPE_OBJECT)

namespace {

simple_query_linux::NativeQueryHostApiImpl* GetHostApi(gpointer user_data) {
  return SIMPLE_QUERY_LINUX_PLUGIN(user_data)->host_api;
}

template <typename Response>
Response* ErrorResponse(
    const simple_query_linux::NativeError& error,
    Response* (*constructor)(const gchar*, const gchar*, FlValue*)) {
  return constructor(error.code.c_str(), error.message.c_str(), nullptr);
}

SqlqNativeQueryHostApiGetCapabilitiesResponse* HandleGetCapabilities(
    gpointer user_data) {
  auto result = GetHostApi(user_data)->GetCapabilities();
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_get_capabilities_response_new_error);
  }
  return sqlq_native_query_host_api_get_capabilities_response_new(
      result.value.get());
}

SqlqNativeQueryHostApiQueryResponse* HandleQuery(FlValue* request,
                                                 gpointer user_data) {
  auto result = GetHostApi(user_data)->Query(request);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_query_response_new_error);
  }
  return sqlq_native_query_host_api_query_response_new(result.value.get());
}

SqlqNativeQueryHostApiMutateResponse* HandleMutate(FlValue* request,
                                                   gpointer user_data) {
  auto result = GetHostApi(user_data)->Mutate(request);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_mutate_response_new_error);
  }
  return sqlq_native_query_host_api_mutate_response_new(result.value.get());
}

SqlqNativeQueryHostApiBatchResponse* HandleBatch(FlValue* request,
                                                 gpointer user_data) {
  auto result = GetHostApi(user_data)->Batch(request);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_batch_response_new_error);
  }
  return sqlq_native_query_host_api_batch_response_new(result.value.get());
}

SqlqNativeQueryHostApiObserveStartResponse* HandleObserveStart(
    FlValue* request, gpointer user_data) {
  auto result = GetHostApi(user_data)->ObserveStart(request);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_observe_start_response_new_error);
  }
  return sqlq_native_query_host_api_observe_start_response_new(
      result.value.c_str());
}

SqlqNativeQueryHostApiObserveStopResponse* HandleObserveStop(
    const gchar* observer_id, gpointer user_data) {
  auto error = GetHostApi(user_data)->ObserveStop(observer_id);
  if (error.has_value()) {
    return ErrorResponse(*error,
                         sqlq_native_query_host_api_observe_stop_response_new_error);
  }
  return sqlq_native_query_host_api_observe_stop_response_new();
}

SqlqNativeQueryHostApiOpenBinaryResponse* HandleOpenBinary(
    FlValue* request, gpointer user_data) {
  auto result = GetHostApi(user_data)->OpenBinary(request);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_open_binary_response_new_error);
  }
  return sqlq_native_query_host_api_open_binary_response_new(result.value.get());
}

SqlqNativeQueryHostApiCloseBinaryResponse* HandleCloseBinary(
    const gchar* handle_id, gpointer user_data) {
  auto error = GetHostApi(user_data)->CloseBinary(handle_id);
  if (error.has_value()) {
    return ErrorResponse(*error,
                         sqlq_native_query_host_api_close_binary_response_new_error);
  }
  return sqlq_native_query_host_api_close_binary_response_new();
}

SqlqNativeQueryHostApiCallExtensionResponse* HandleCallExtension(
    const gchar* name_space, const gchar* method, FlValue* args,
    gpointer user_data) {
  auto result =
      GetHostApi(user_data)->CallExtension(name_space, method, args);
  if (result.error.has_value()) {
    return ErrorResponse(*result.error,
                         sqlq_native_query_host_api_call_extension_response_new_error);
  }
  return sqlq_native_query_host_api_call_extension_response_new(
      result.value.get());
}

const SqlqNativeQueryHostApiVTable kHostApiVTable = {
    HandleGetCapabilities, HandleQuery,        HandleMutate,
    HandleBatch,           HandleObserveStart, HandleObserveStop,
    HandleOpenBinary,      HandleCloseBinary,  HandleCallExtension,
};

}  // namespace

static void simple_query_linux_plugin_dispose(GObject* object) {
  auto* self = SIMPLE_QUERY_LINUX_PLUGIN(object);
  if (self->registrar != nullptr) {
    sqlq_native_query_host_api_clear_method_handlers(
        fl_plugin_registrar_get_messenger(self->registrar), nullptr);
  }
  delete self->host_api;
  self->host_api = nullptr;
  g_clear_object(&self->registrar);
  G_OBJECT_CLASS(simple_query_linux_plugin_parent_class)->dispose(object);
}

static void simple_query_linux_plugin_class_init(
    SimpleQueryLinuxPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = simple_query_linux_plugin_dispose;
}

static void simple_query_linux_plugin_init(SimpleQueryLinuxPlugin* self) {
  self->registrar = nullptr;
  self->host_api = nullptr;
}

void simple_query_linux_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  auto* plugin = SIMPLE_QUERY_LINUX_PLUGIN(
      g_object_new(simple_query_linux_plugin_get_type(), nullptr));
  plugin->registrar = FL_PLUGIN_REGISTRAR(g_object_ref(registrar));
  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  plugin->host_api =
      new simple_query_linux::NativeQueryHostApiImpl(messenger);
  sqlq_native_query_host_api_set_method_handlers(
      messenger, nullptr, &kHostApiVTable, g_object_ref(plugin),
      g_object_unref);
  g_object_unref(plugin);
}
