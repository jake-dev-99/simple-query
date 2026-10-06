#include "include/simple_query_linux/simple_query_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
  /**
   * Purpose: Release a uniquely owned Flutter value through its C API.
   * @param value is the nullable Flutter value leaving scope.
   * @returns Nothing.
   * @throws Nothing.
   */
  void operator()(FlValue* value) const {
    if (value != nullptr) {
      fl_value_unref(value);
    }
  }
};

using ValuePtr = std::unique_ptr<FlValue, FlValueDeleter>;
using Rows = std::vector<ValuePtr>;

/** Purpose: Adopt an owned Flutter value. @param value is transferred to the
 * returned pointer. @returns An RAII Flutter value. @throws Nothing. */
ValuePtr Value(FlValue* value) { return ValuePtr(value); }

/** Purpose: Transfer a value into a string-keyed Flutter map. @param map is
 * mutated. @param key identifies the entry. @param value is transferred.
 * @returns Nothing. @throws Nothing. */
void MapSet(FlValue* map, const char* key, FlValue* value) {
  fl_value_set_string_take(map, key, value);
}

/** Purpose: Store a copied string in a Flutter map. @param map is mutated.
 * @param key identifies the entry. @param value supplies the string.
 * @returns Nothing. @throws Nothing. */
void MapSetString(FlValue* map, const char* key, const std::string& value) {
  MapSet(map, key, fl_value_new_string(value.c_str()));
}

/** Purpose: Store a Boolean in a Flutter map. @param map is mutated. @param key
 * identifies the entry. @param value supplies the Boolean. @returns Nothing.
 * @throws Nothing. */
void MapSetBool(FlValue* map, const char* key, bool value) {
  MapSet(map, key, fl_value_new_bool(value));
}

/** Purpose: Store an integer in a Flutter map. @param map is mutated. @param key
 * identifies the entry. @param value supplies the integer. @returns Nothing.
 * @throws Nothing. */
void MapSetInt(FlValue* map, const char* key, int64_t value) {
  MapSet(map, key, fl_value_new_int(value));
}

/** Purpose: Safely look up a string key only when the input is a map.
 * @param map is the nullable candidate map. @param key identifies the entry.
 * @returns A borrowed value or null. @throws Nothing. */
FlValue* FindValue(FlValue* map, const std::string& key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }
  return fl_value_lookup_string(map, key.c_str());
}

/** Purpose: Normalize scalar Flutter values to text for query comparison.
 * @param value is the nullable scalar. @returns Text or null for unsupported
 * types. @throws std::bad_alloc when text allocation fails. */
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

/** Purpose: Read a map string while preserving an explicit fallback.
 * @param map supplies the value. @param key identifies it. @param fallback is
 * used when conversion fails. @returns The converted or fallback text.
 * @throws std::bad_alloc when text allocation fails. */
std::string StringOr(FlValue* map, const std::string& key,
                     const std::string& fallback) {
  return AsString(FindValue(map, key)).value_or(fallback);
}

/** Purpose: Normalize numeric Flutter values to a signed integer.
 * @param value is the nullable scalar. @returns An integer or null for an
 * unsupported type. @throws Nothing. */
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

/** Purpose: Read a strict Boolean map field without coercion. @param map
 * supplies the value. @param key identifies it. @param fallback is used when
 * absent or mistyped. @returns The decoded or fallback Boolean.
 * @throws Nothing. */
bool BoolOr(FlValue* map, const std::string& key, bool fallback) {
  FlValue* value = FindValue(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
    return fallback;
  }
  return fl_value_get_bool(value);
}

/** Purpose: Validate a Flutter value as a map. @param value is borrowed.
 * @returns The same borrowed value or null. @throws Nothing. */
FlValue* AsMap(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_MAP
             ? value
             : nullptr;
}

/** Purpose: Validate a Flutter value as a list. @param value is borrowed.
 * @returns The same borrowed value or null. @throws Nothing. */
FlValue* AsList(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_LIST
             ? value
             : nullptr;
}

/** Purpose: Copy a Flutter map container while retaining each mapped value.
 * @param map is borrowed and may be null or mistyped. @returns A new map.
 * @throws Nothing. */
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

/**
 * Purpose: Give an observer thread a fully independent FlValue because
 * Flutter Linux FlValue reference counts are not atomic.
 * @param value is encoded and decoded on the platform thread.
 * @param error_message receives a stable simple_query failure on codec error.
 * @returns A deep copy safe for exclusive worker-thread ownership.
 * @throws std::bad_alloc if the returned string cannot be allocated.
 */
ValuePtr CopyValueForWorker(FlValue* value, std::string* error_message) {
  g_autoptr(FlStandardMessageCodec) codec =
      fl_standard_message_codec_new();
  g_autoptr(GError) error = nullptr;
  g_autoptr(GBytes) encoded = fl_message_codec_encode_message(
      FL_MESSAGE_CODEC(codec), value, &error);
  if (encoded == nullptr) {
    *error_message =
        std::string("simple_query: could not copy observation request - ") +
        (error != nullptr ? error->message : "encoding failed");
    return ValuePtr();
  }
  FlValue* copy = fl_message_codec_decode_message(
      FL_MESSAGE_CODEC(codec), encoded, &error);
  if (copy == nullptr) {
    *error_message =
        std::string("simple_query: could not copy observation request - ") +
        (error != nullptr ? error->message : "decoding failed");
    return ValuePtr();
  }
  return Value(copy);
}

/** Purpose: Normalize ASCII metadata for case-insensitive matching.
 * @param value is copied for in-place normalization. @returns Lowercase text.
 * @throws Nothing after the input copy succeeds. */
std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

/** Purpose: Infer the stable MIME value exposed for a filesystem record.
 * @param path supplies the filename extension. @returns A known MIME type or
 * application/octet-stream. @throws std::bad_alloc on string allocation. */
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

/** Purpose: Decide whether a MIME value belongs to the media query domain.
 * @param mime is the normalized MIME value. @returns True for image, video, or
 * audio prefixes. @throws Nothing. */
bool IsMediaMime(const std::string& mime) {
  return mime.rfind("image/", 0) == 0 || mime.rfind("video/", 0) == 0 ||
         mime.rfind("audio/", 0) == 0;
}

/** Purpose: Project a MIME value to the portable media type field.
 * @param mime is the normalized MIME value. @returns image, video, audio, or
 * other. @throws Nothing. */
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

/** Purpose: Discover EDS sources through the session-bus object manager.
 * @returns Source descriptors or a stable unavailable diagnostic.
 * @throws std::bad_alloc when result allocation fails. */
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

/** Purpose: Read portable contact rows from libebook or EDS discovery.
 * @returns Contact rows or a stable source error. @throws std::bad_alloc when
 * result allocation fails. */
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

/** Purpose: Read portable event rows from libecal or EDS discovery.
 * @returns Calendar rows or a stable source error. @throws std::bad_alloc when
 * result allocation fails. */
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

/** Purpose: Filter discovered EDS sources for one extension API.
 * @param address_books selects address books instead of calendars.
 * @returns Matching sources or the discovery error. @throws std::bad_alloc
 * when result allocation fails. */
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

/**
 * Purpose: Build the stable unavailable detail used for filesystem failures.
 * @param operation describes the failed filesystem action.
 * @param path identifies the record that could not be inspected.
 * @param error contains the operating-system failure.
 * @returns A simple_query-prefixed diagnostic safe to cross the host boundary.
 * @throws Nothing.
 */
std::string FileSystemFailure(const char* operation,
                              const std::filesystem::path& path,
                              const std::error_code& error) {
  return "simple_query: " + std::string(operation) + " failed for " +
         path.string() + " - " + error.message();
}

/**
 * Purpose: Read a file timestamp without allowing filesystem exceptions to
 * escape a Linux HostApi callback or observation worker.
 * @param entry is the directory record being inspected.
 * @param error receives any operating-system failure.
 * @returns The implementation-defined file timestamp in milliseconds.
 * @throws Nothing.
 */
int64_t ModifiedEpochMs(const std::filesystem::directory_entry& entry,
                        std::error_code& error) {
  const auto ticks = entry.last_write_time(error).time_since_epoch();
  if (error) {
    return 0;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count();
}

/**
 * Purpose: Encode observation time in the ISO-8601 UTC format Dart requires.
 * @returns A timestamp with fixed millisecond precision and a literal Z suffix.
 * @throws std::bad_alloc if the returned string cannot be allocated.
 */
std::string IsoUtcTimestamp() {
  g_autoptr(GDateTime) now = g_date_time_new_now_utc();
  g_autofree gchar* prefix =
      g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
  g_autofree gchar* suffix =
      g_strdup_printf(".%03dZ", g_date_time_get_microsecond(now) / 1000);
  return std::string(prefix) + suffix;
}

/** Purpose: Read a record field as comparable text. @param row is the record.
 * @param key selects its field. @returns Converted text or an empty string.
 * @throws std::bad_alloc when text allocation fails. */
std::string ValueAsString(FlValue* row, const std::string& key) {
  return AsString(FindValue(row, key)).value_or("");
}

/** Purpose: Encode one runtime capability descriptor. @param domain names the
 * domain. @param can_read permits reads. @param can_write permits writes.
 * @param can_observe permits observation. @param can_stream permits binary
 * streams. @param reason explains a restriction. @returns A new descriptor.
 * @throws Nothing. */
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

/** Purpose: Resolve a requested filesystem root or the current directory.
 * @param request may contain platformData.rootPath. @returns The selected path.
 * @throws std::filesystem::filesystem_error when current-directory lookup
 * fails, and std::bad_alloc on path allocation. */
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

/**
 * Purpose: Enumerate files without converting access failures into empty data.
 * @param root is the filesystem root selected by the caller.
 * @param media_only filters results to supported media MIME types.
 * @returns Rows on success or a stable simple_query error on inspection failure.
 * @throws Nothing.
 */
DomainRowsResult ListRecords(const std::filesystem::path& root,
                             bool media_only) {
  DomainRowsResult result;
  std::error_code error;
  const bool root_exists = std::filesystem::exists(root, error);
  if (error) {
    result.error = FileSystemFailure("filesystem query", root, error);
    return result;
  }
  if (!root_exists) {
    return result;
  }

  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::none, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    result.error = FileSystemFailure("filesystem query", root, error);
    return result;
  }
  while (iterator != end) {
    const auto entry = *iterator;
    const auto path = entry.path();
    error.clear();
    const bool is_directory = entry.is_directory(error);
    if (error) {
      result.error = FileSystemFailure("filesystem metadata", path, error);
      return result;
    }

    const std::string mime = MimeFromPath(path);
    auto row = Value(fl_value_new_map());
    if (media_only) {
      if (is_directory || !IsMediaMime(mime)) {
        iterator.increment(error);
        if (error) {
          result.error = FileSystemFailure("filesystem query", root, error);
          return result;
        }
        continue;
      }
      MapSetString(row.get(), "id", path.string());
      MapSetString(row.get(), "uriOrPath", path.string());
      MapSetString(row.get(), "mediaType", MediaType(mime));
      MapSetString(row.get(), "mimeType", mime);
      const bool is_regular = entry.is_regular_file(error);
      if (error) {
        result.error = FileSystemFailure("filesystem metadata", path, error);
        return result;
      }
      if (is_regular) {
        MapSetInt(row.get(), "size",
                  static_cast<int64_t>(entry.file_size(error)));
        if (error) {
          result.error = FileSystemFailure("filesystem size", path, error);
          return result;
        }
      }
      const auto modified = ModifiedEpochMs(entry, error);
      if (error) {
        result.error = FileSystemFailure("filesystem timestamp", path, error);
        return result;
      }
      MapSetString(row.get(), "createdAt", std::to_string(modified));
      MapSetString(row.get(), "modifiedAt", std::to_string(modified));
      result.rows.push_back(std::move(row));
      iterator.increment(error);
      if (error) {
        result.error = FileSystemFailure("filesystem query", root, error);
        return result;
      }
      continue;
    }

    MapSetString(row.get(), "id", path.string());
    MapSetString(row.get(), "path", path.string());
    MapSetString(row.get(), "name", path.filename().string());
    MapSetBool(row.get(), "isDirectory", is_directory);
    const bool is_regular = entry.is_regular_file(error);
    if (error) {
      result.error = FileSystemFailure("filesystem metadata", path, error);
      return result;
    }
    if (is_regular) {
      MapSetInt(row.get(), "size",
                static_cast<int64_t>(entry.file_size(error)));
      if (error) {
        result.error = FileSystemFailure("filesystem size", path, error);
        return result;
      }
    }
    const auto modified = ModifiedEpochMs(entry, error);
    if (error) {
      result.error = FileSystemFailure("filesystem timestamp", path, error);
      return result;
    }
    MapSetString(row.get(), "modifiedAt", std::to_string(modified));
    MapSetString(row.get(), "mimeType", mime);
    MapSetString(row.get(), "type", is_directory ? "directory" : "file");
    MapSetString(row.get(), "extension", path.extension().string());
    MapSetInt(row.get(), "modifiedEpochMs", modified);
    result.rows.push_back(std::move(row));
    iterator.increment(error);
    if (error) {
      result.error = FileSystemFailure("filesystem query", root, error);
      return result;
    }
  }

  return result;
}

/** Purpose: Apply portable equality, containment, and list filters in order.
 * @param rows owns candidate records. @param filters is the borrowed filter
 * list. @returns The retained records. @throws std::bad_alloc on allocation. */
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

/** Purpose: Apply the first requested portable sort to native records.
 * @param rows owns records reordered in place. @param sort is the borrowed sort
 * list. @returns Nothing. @throws std::bad_alloc during text comparison. */
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

/** Purpose: Project native rows to the exact requested field set.
 * @param rows supplies source records. @param projection lists field names.
 * @returns A new Flutter list of projected records. @throws Nothing. */
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

struct SnapshotResult {
  Snapshot snapshot;
  std::optional<std::string> error;
};

/**
 * Purpose: Build an observer snapshot while preserving data-source failures.
 * @param request is the observer's independent request value.
 * @param domain selects the native record source.
 * @returns A snapshot or a stable simple_query error; errors are never treated
 * as an empty snapshot.
 * @throws Nothing.
 */
SnapshotResult BuildSnapshotForDomain(FlValue* request,
                                      const std::string& domain) {
  SnapshotResult result;
  DomainRowsResult records;
  if (domain == "files" || domain == "media") {
    records = ListRecords(ResolveRootPath(request), domain == "media");
  } else if (domain == "contacts") {
    records = ListContactRecords();
  } else if (domain == "calendar") {
    records = ListCalendarRecords();
  }
  if (records.error.has_value()) {
    result.error = records.error;
    return result;
  }

  for (const auto& row : records.rows) {
    const auto id = AsString(FindValue(row.get(), "id"));
    if (!id.has_value()) {
      continue;
    }
    const int64_t modified =
        AsInt(FindValue(row.get(), "modifiedEpochMs"))
            .value_or(AsInt(FindValue(row.get(), "updatedAt"))
                          .value_or(AsInt(FindValue(row.get(), "startAt"))
                                        .value_or(0)));
    result.snapshot[*id] = modified;
  }
  return result;
}

/** Purpose: Identify records added, removed, or changed between snapshots.
 * @param previous is the last delivered snapshot. @param current is the newest
 * snapshot. @returns A new Flutter list of changed IDs. @throws Nothing. */
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

struct CancellableDeleter {
  /** Purpose: Release an exclusively owned GLib cancellation token.
   * @param cancellable is the nullable token leaving scope.
   * @returns Nothing. @throws Nothing. */
  void operator()(GCancellable* cancellable) const {
    if (cancellable != nullptr) {
      g_object_unref(cancellable);
    }
  }
};

using CancellablePtr = std::unique_ptr<GCancellable, CancellableDeleter>;

/** Purpose: Own one observer's worker, cancellation, and queued deliveries.
 * Ownership: The registry and asynchronous deliveries share this state; its
 * cancellable is released automatically if startup never reaches the registry.
 */
struct ObserverState {
  std::atomic<bool> active{true};
  std::atomic<bool> delivery_pending{false};
  std::thread worker;
  std::condition_variable wakeup;
  std::mutex wakeup_mutex;
  std::mutex sources_mutex;
  std::set<GSource*> pending_sources;
  CancellablePtr cancellable;
};

struct NativeError {
  std::string code;
  std::string message;
};

struct ValueResult {
  ValuePtr value;
  std::optional<NativeError> error;
};

/** Purpose: Wrap an owned Flutter payload as a successful native result.
 * @param value is transferred. @returns A success result. @throws Nothing. */
ValueResult Success(ValuePtr value) {
  return ValueResult{std::move(value), std::nullopt};
}

/** Purpose: Wrap a stable code and message as a native operation failure.
 * @param code is the transport error code. @param message is user-readable.
 * @returns A failed native result. @throws std::bad_alloc while copying text. */
ValueResult Failure(const std::string& code, const std::string& message) {
  return ValueResult{ValuePtr(), NativeError{code, message}};
}

/**
 * Purpose: Convert native transport codes to public SimpleQueryErrorCode names.
 * @param code is the kebab-case native error code.
 * @returns The camel-case code name, or null when native code drift is found.
 * @throws Nothing.
 */
std::optional<std::string> PortableErrorCodeName(const std::string& code) {
  if (code == "not-supported") return "notSupported";
  if (code == "permission-denied") return "permissionDenied";
  if (code == "invalid-query") return "invalidQuery";
  if (code == "transient") return "transientFailure";
  if (code == "unavailable") return "unavailable";
  return std::nullopt;
}

/**
 * Purpose: Attach sequential-best-effort metadata to one ordered batch result.
 * @param operation supplies the domain for structured error context.
 * @param result is the mutation value or native failure for this operation.
 * @returns A successful or failed MutationResult-shaped payload.
 * @throws Nothing.
 */
ValuePtr BatchOperationResult(FlValue* operation, ValueResult result) {
  auto payload = result.value != nullptr ? std::move(result.value)
                                         : Value(fl_value_new_map());
  if (result.error.has_value()) {
    MapSetInt(payload.get(), "affectedCount", 0);
  }
  auto metadata = CloneMap(AsMap(FindValue(payload.get(), "metadata")));
  MapSetString(metadata.get(), "batchSemantics", "sequentialBestEffort");
  MapSetString(metadata.get(), "implementation", "native_linux");
  if (result.error.has_value()) {
    auto error = Value(fl_value_new_map());
    const auto portable_code = PortableErrorCodeName(result.error->code);
    MapSetString(error.get(), "code",
                 portable_code.value_or("unavailable"));
    const std::string message =
        portable_code.has_value()
            ? result.error->message
            : result.error->message + " (unrecognized native error code: " +
                  result.error->code + ")";
    MapSetString(error.get(), "message", message);
    MapSetString(error.get(), "domain",
                 StringOr(operation, "domain", "platformSpecific"));
    MapSetString(error.get(), "operation", "write");
    MapSet(metadata.get(), "error", error.release());
  }
  MapSet(payload.get(), "metadata", metadata.release());
  return payload;
}

struct StringResult {
  std::string value;
  std::optional<NativeError> error;
};

}  // namespace

class NativeQueryHostApiImpl {
 public:
  /**
   * Purpose: Bind Linux host behavior to Dart and capture the platform GLib
   * context used for all future Flutter engine interaction.
   * @param messenger is the registrar-owned Flutter binary messenger.
   * @throws Nothing.
   */
  explicit NativeQueryHostApiImpl(FlBinaryMessenger* messenger)
      : flutter_api_(sqlq_native_query_flutter_api_new(messenger, nullptr)),
        platform_context_(g_main_context_ref_thread_default()) {}

  /**
   * Purpose: Stop workers, cancel deliveries, and release platform resources.
   * @throws Nothing.
   */
  ~NativeQueryHostApiImpl() {
    ShutdownObservers();
    g_clear_object(&flutter_api_);
    g_clear_pointer(&platform_context_, g_main_context_unref);
  }

  /** Purpose: Describe currently reachable Linux domains and extensions.
   * @returns A portable capability snapshot. @throws std::bad_alloc when the
   * snapshot cannot be allocated. */
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

  /** Purpose: Execute one validated native read and portable result projection.
   * @param request is the decoded Pigeon query payload. @returns Query records
   * or a structured native error. @throws Native allocation or filesystem
   * exceptions for the outer callback boundary to translate. */
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
      auto file_rows =
          ListRecords(ResolveRootPath(request), domain == "media");
      if (file_rows.error.has_value()) {
        return Failure("unavailable", *file_rows.error);
      }
      rows = std::move(file_rows.rows);
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

  /** Purpose: Execute one filesystem mutation with fail-closed I/O handling.
   * @param request is the decoded Pigeon mutation payload. @returns A mutation
   * result or structured native error. @throws Native allocation exceptions
   * for the outer callback boundary to translate. */
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
      const std::filesystem::path output_path(*path);
      if (BoolOr(values, "isDirectory", false)) {
        std::filesystem::create_directories(output_path, error);
        if (error) {
          return Failure(
              "unavailable",
              FileSystemFailure("filesystem directory creation", output_path,
                                error));
        }
      } else {
        const auto parent_path = output_path.parent_path();
        if (!parent_path.empty()) {
          std::filesystem::create_directories(parent_path, error);
          if (error) {
            return Failure(
                "unavailable",
                FileSystemFailure("filesystem directory creation",
                                  parent_path, error));
          }
        }
        std::ofstream stream(output_path, std::ios::binary);
        if (!stream.is_open()) {
          return Failure("unavailable",
                         "simple_query: could not create output file " +
                             output_path.string());
        }
        stream << AsString(FindValue(values, "content")).value_or("");
        stream.flush();
        if (!stream.good()) {
          return Failure("unavailable",
                         "simple_query: could not write output file " +
                             output_path.string());
        }
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
          const auto removed = std::filesystem::remove_all(*path, error);
          if (error) {
            return Failure(
                "unavailable",
                FileSystemFailure("filesystem delete", *path, error));
          }
          deleted += static_cast<int64_t>(removed);
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
        const bool exists = std::filesystem::exists(effective_path, error);
        if (error) {
          return Failure(
              "unavailable",
              FileSystemFailure("filesystem existence check", effective_path,
                                error));
        }
        if (!exists) {
          continue;
        }

        bool changed = false;
        const auto new_path = AsString(FindValue(values, "newPath"));
        if (new_path.has_value() && !new_path->empty() &&
            *new_path != original_path) {
          const std::filesystem::path next_path(*new_path);
          const auto parent_path = next_path.parent_path();
          if (!parent_path.empty()) {
            std::filesystem::create_directories(parent_path, error);
            if (error) {
              return Failure(
                  "unavailable",
                  FileSystemFailure("filesystem directory creation",
                                    parent_path, error));
            }
          }
          std::filesystem::rename(effective_path, next_path, error);
          if (error) {
            return Failure(
                "unavailable",
                FileSystemFailure("filesystem rename", effective_path,
                                  error));
          }
          effective_path = next_path;
          changed = true;
        }

        const bool is_directory =
            std::filesystem::is_directory(effective_path, error);
        if (error) {
          return Failure(
              "unavailable",
              FileSystemFailure("filesystem metadata", effective_path,
                                error));
        }
        if (!is_directory) {
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
            stream.flush();
            if (!stream.good()) {
              return Failure("unavailable",
                             "simple_query: could not write file " +
                                 effective_path.string());
            }
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
            stream.flush();
            if (!stream.good()) {
              return Failure("unavailable",
                             "simple_query: could not write file " +
                                 effective_path.string());
            }
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

  /** Purpose: Execute every well-formed mutation in stable input order.
   * @param request is the decoded batch payload. @returns One annotated result
   * per operation, preserving failures. @throws Native allocation exceptions
   * before or after per-operation containment. */
  ValueResult Batch(FlValue* request) {
    FlValue* operations = AsList(FindValue(request, "operations"));
    if (operations == nullptr) {
      return Failure("invalid-query",
                     "simple_query: batch requires operations");
    }

    for (size_t index = 0; index < fl_value_get_length(operations); index++) {
      if (AsMap(fl_value_get_list_value(operations, index)) == nullptr) {
        return Failure("invalid-query",
                       "simple_query: batch operation must be a map");
      }
    }

    auto results = Value(fl_value_new_list());
    for (size_t index = 0; index < fl_value_get_length(operations); index++) {
      FlValue* operation = AsMap(fl_value_get_list_value(operations, index));
      auto merged = CloneMap(operation);
      if (FindValue(operation, "platformData") == nullptr &&
          FindValue(request, "platformData") != nullptr) {
        MapSet(merged.get(), "platformData",
               fl_value_ref(FindValue(request, "platformData")));
      }

      ValueResult result;
      try {
        result = Mutate(merged.get());
      } catch (const std::exception& error) {
        result = Failure(
            "unavailable",
            "simple_query: Linux batch operation failed - " +
                std::string(error.what()));
      } catch (...) {
        result = Failure("unavailable",
                         "simple_query: Linux batch operation failed");
      }
      fl_value_append_take(
          results.get(),
          BatchOperationResult(operation, std::move(result)).release());
    }

    auto response = Value(fl_value_new_map());
    MapSet(response.get(), "results", results.release());
    return Success(std::move(response));
  }

  /** Purpose: Start one cancellable polling worker for a supported domain.
   * @param request is the decoded observation payload. @returns Its observer ID
   * or a structured error. @throws Thread or allocation exceptions for the
   * outer callback boundary to translate. */
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
    std::string copy_error;
    auto request_copy = CopyValueForWorker(request, &copy_error);
    if (request_copy == nullptr) {
      return StringResult{"", NativeError{"unavailable", copy_error}};
    }
    StringResult response{observer_id, std::nullopt};
    auto state = std::make_shared<ObserverState>();
    state->cancellable.reset(g_cancellable_new());
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      observers_.emplace(observer_id, state);
    }

    try {
      state->worker = std::thread(
          [this, observer_id, domain, request = std::move(request_copy),
           interval_ms, state]() mutable {
            RunObserver(observer_id, domain, std::move(request), interval_ms,
                        state);
          });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        observers_.erase(observer_id);
      }
      StopObserver(state);
      throw;
    }
    return response;
  }

  /** Purpose: Stop and remove one observer without failing repeated cleanup.
   * @param observer_id selects the observer. @returns No error after cleanup.
   * @throws Nothing after lookup succeeds. */
  std::optional<NativeError> ObserveStop(const std::string& observer_id) {
    std::shared_ptr<ObserverState> state;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      const auto found = observers_.find(observer_id);
      if (found == observers_.end()) {
        return std::nullopt;
      }
      state = found->second;
      observers_.erase(found);
    }
    StopObserver(state);
    return std::nullopt;
  }

  /** Purpose: Validate and expose one filesystem resource as a binary handle.
   * @param request identifies the native resource. @returns Handle metadata or
   * a structured error. @throws Native allocation exceptions for the outer
   * callback boundary to translate. */
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

  /** Purpose: Release one binary handle idempotently. @param handle_id selects
   * the handle. @returns No error after cleanup. @throws Nothing. */
  std::optional<NativeError> CloseBinary(const std::string& handle_id) {
    open_handles_.erase(handle_id);
    return std::nullopt;
  }

  /** Purpose: Dispatch a namespaced Linux diagnostic extension.
   * @param name_space selects the extension. @param method selects its action.
   * @param args contains optional decoded arguments. @returns Extension data or
   * a structured error. @throws Native allocation exceptions for the outer
   * callback boundary to translate. */
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
  struct ObserveDelivery {
    NativeQueryHostApiImpl* host;
    std::shared_ptr<ObserverState> state;
    std::string observer_id;
    ValuePtr event;
    GSource* source;
    bool started = false;
  };

  struct ObserveCompletion {
    std::shared_ptr<ObserverState> state;
    std::string observer_id;
  };

  /**
   * Purpose: Release a queued delivery and reopen the bounded delivery slot
   * when it was destroyed before reaching Flutter.
   * @param user_data is the ObserveDelivery owned by one GLib source.
   * @returns Nothing.
   * @throws Nothing.
   */
  static void DestroyObserveDelivery(gpointer user_data) {
    std::unique_ptr<ObserveDelivery> delivery(
        static_cast<ObserveDelivery*>(user_data));
    if (!delivery->started) {
      delivery->state->delivery_pending.store(false);
    }
  }

  /**
   * Purpose: Finish every generated FlutterApi send and surface Dart or
   * transport rejection without retaining the plugin host.
   * @param object is the generated FlutterApi that initiated the send.
   * @param result is the generated asynchronous result.
   * @param user_data owns observer completion context.
   * @returns Nothing.
   * @throws Nothing.
   */
  static void FinishObserveDelivery(GObject* object, GAsyncResult* result,
                                    gpointer user_data) {
    std::unique_ptr<ObserveCompletion> completion(
        static_cast<ObserveCompletion*>(user_data));
    g_autoptr(GError) error = nullptr;
    g_autoptr(SqlqNativeQueryFlutterApiOnObserveEventResponse) response =
        sqlq_native_query_flutter_api_on_observe_event_finish(
            SQLQ_NATIVE_QUERY_FLUTTER_API(object), result, &error);
    if (error != nullptr &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("simple_query: observer %s delivery failed - %s",
                completion->observer_id.c_str(), error->message);
    } else if (response == nullptr && error == nullptr) {
      g_warning("simple_query: observer %s delivery returned no response",
                completion->observer_id.c_str());
    } else if (response != nullptr &&
               sqlq_native_query_flutter_api_on_observe_event_response_is_error(
                   response)) {
      g_warning("simple_query: observer %s delivery rejected (%s) - %s",
                completion->observer_id.c_str(),
                sqlq_native_query_flutter_api_on_observe_event_response_get_error_code(
                    response),
                sqlq_native_query_flutter_api_on_observe_event_response_get_error_message(
                    response));
    }
    completion->state->delivery_pending.store(false);
  }

  /**
   * Purpose: Run a queued observation delivery only when the platform context
   * advances, never synchronously on the polling worker.
   * @param user_data is the ObserveDelivery attached to the idle source.
   * @returns G_SOURCE_REMOVE after starting at most one FlutterApi send.
   * @throws Nothing.
   */
  static gboolean DeliverObserveEvent(gpointer user_data) {
    auto* delivery = static_cast<ObserveDelivery*>(user_data);
    {
      std::lock_guard<std::mutex> lock(delivery->state->sources_mutex);
      if (delivery->state->pending_sources.erase(delivery->source) != 0) {
        g_source_unref(delivery->source);
      }
    }
    if (!delivery->state->active.load()) {
      return G_SOURCE_REMOVE;
    }
    delivery->started = true;
    auto* completion = new ObserveCompletion{delivery->state,
                                              delivery->observer_id};
    sqlq_native_query_flutter_api_on_observe_event(
        delivery->host->flutter_api_, delivery->observer_id.c_str(),
        delivery->event.get(), delivery->state->cancellable.get(),
        FinishObserveDelivery, completion);
    return G_SOURCE_REMOVE;
  }

  /**
   * Purpose: Bound each observer to one queued or in-flight event and attach
   * the queued work explicitly to the captured platform context.
   * @param observer_id identifies the event recipient.
   * @param state owns cancellation and pending-source tracking.
   * @param event is transferred into the queued delivery.
   * @returns True when the event was queued; false under backpressure or stop.
   * @throws Nothing.
   */
  bool QueueObserveEvent(const std::string& observer_id,
                         const std::shared_ptr<ObserverState>& state,
                         ValuePtr event) {
    bool expected = false;
    if (!state->active.load() || !state->delivery_pending.compare_exchange_strong(
                                    expected, true)) {
      return false;
    }
    GSource* source = g_idle_source_new();
    auto* delivery = new ObserveDelivery{this, state, observer_id,
                                         std::move(event), source};
    g_source_set_callback(source, DeliverObserveEvent, delivery,
                          DestroyObserveDelivery);
    {
      std::lock_guard<std::mutex> lock(state->sources_mutex);
      if (!state->active.load()) {
        g_source_unref(source);
        return false;
      }
      state->pending_sources.insert(source);
      g_source_attach(source, platform_context_);
    }
    return true;
  }

  /**
   * Purpose: Poll one native domain without letting failures escape the worker
   * or masquerade as an empty snapshot.
   * @param observer_id identifies log and callback context.
   * @param domain selects the native source.
   * @param request is exclusively owned by this worker.
   * @param interval_ms is the polling cadence.
   * @param state owns cancellation and lifecycle coordination.
   * @returns Nothing.
   * @throws Nothing.
   */
  void RunObserver(const std::string& observer_id, const std::string& domain,
                   ValuePtr request, int64_t interval_ms,
                   const std::shared_ptr<ObserverState>& state) noexcept {
    try {
      auto previous = BuildSnapshotForDomain(request.get(), domain);
      if (previous.error.has_value()) {
        g_warning("simple_query: observer %s stopped - %s",
                  observer_id.c_str(), previous.error->c_str());
        state->active.store(false);
        return;
      }
      while (state->active.load()) {
        std::unique_lock<std::mutex> lock(state->wakeup_mutex);
        if (state->wakeup.wait_for(
                lock, std::chrono::milliseconds(interval_ms),
                [&state] { return !state->active.load(); })) {
          break;
        }
        lock.unlock();
        auto current = BuildSnapshotForDomain(request.get(), domain);
        if (current.error.has_value()) {
          g_warning("simple_query: observer %s stopped - %s",
                    observer_id.c_str(), current.error->c_str());
          state->active.store(false);
          return;
        }
        if (current.snapshot == previous.snapshot) {
          continue;
        }
        auto event = Value(fl_value_new_map());
        MapSetString(event.get(), "domain", domain);
        MapSetString(event.get(), "changeType", "unknown");
        MapSetString(event.get(), "timestamp", IsoUtcTimestamp());
        MapSet(event.get(), "ids",
               ChangedIds(previous.snapshot, current.snapshot).release());
        MapSetString(event.get(), "source", "linux-host");
        if (QueueObserveEvent(observer_id, state, std::move(event))) {
          previous = std::move(current);
        }
      }
    } catch (const std::exception& error) {
      g_warning("simple_query: observer %s stopped - %s", observer_id.c_str(),
                error.what());
      state->active.store(false);
    } catch (...) {
      g_warning("simple_query: observer %s stopped - unknown native failure",
                observer_id.c_str());
      state->active.store(false);
    }
  }

  /**
   * Purpose: Stop one worker, destroy queued sources, and cancel in-flight
   * delivery before releasing observer state.
   * @param state is the observer removed from the public registry.
   * @returns Nothing.
   * @throws Nothing.
   */
  void StopObserver(const std::shared_ptr<ObserverState>& state) {
    state->active.store(false);
    state->wakeup.notify_all();
    if (state->worker.joinable()) {
      state->worker.join();
    }
    {
      std::lock_guard<std::mutex> lock(state->sources_mutex);
      for (GSource* source : state->pending_sources) {
        g_source_destroy(source);
        g_source_unref(source);
      }
      state->pending_sources.clear();
    }
    if (state->cancellable != nullptr) {
      g_cancellable_cancel(state->cancellable.get());
      state->cancellable.reset();
    }
  }

  /**
   * Purpose: Apply StopObserver to every observer during plugin disposal.
   * @returns Nothing.
   * @throws Nothing.
   */
  void ShutdownObservers() {
    std::vector<std::shared_ptr<ObserverState>> states;
    {
      std::lock_guard<std::mutex> lock(observers_mutex_);
      for (auto& item : observers_) {
        states.push_back(std::move(item.second));
      }
      observers_.clear();
    }
    for (auto& state : states) {
      StopObserver(state);
    }
  }

  SqlqNativeQueryFlutterApi* flutter_api_;
  GMainContext* platform_context_;
  int64_t handle_counter_ = 0;
  int64_t observer_counter_ = 0;
  std::map<std::string, std::string> open_handles_;
  std::map<std::string, std::shared_ptr<ObserverState>> observers_;
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

/** Purpose: Resolve the host implementation retained by a plugin callback.
 * @param user_data owns the registered plugin. @returns Its borrowed host API.
 * @throws Nothing. */
simple_query_linux::NativeQueryHostApiImpl* GetHostApi(gpointer user_data) {
  return SIMPLE_QUERY_LINUX_PLUGIN(user_data)->host_api;
}

/**
 * Purpose: Convert a known native domain error into its generated envelope.
 * @param error supplies the stable code and simple_query-prefixed message.
 * @param constructor creates the method-specific generated response.
 * @returns A newly owned generated error response.
 * @throws Nothing.
 */
template <typename Response>
Response* ErrorResponse(
    const simple_query_linux::NativeError& error,
    Response* (*constructor)(const gchar*, const gchar*, FlValue*)) {
  return constructor(error.code.c_str(), error.message.c_str(), nullptr);
}

/**
 * Purpose: Prevent every C++ exception from crossing a generated C callback.
 * @param callback executes one concrete Linux HostApi operation.
 * @param constructor creates the method-specific generated error response.
 * @returns The operation response or a stable unavailable error response.
 * @throws Nothing; all native exceptions are translated.
 */
template <typename Response, typename Callback>
Response* GuardHostCallback(
    Callback&& callback,
    Response* (*constructor)(const gchar*, const gchar*, FlValue*)) noexcept {
  try {
    return callback();
  } catch (const std::exception& error) {
    g_autofree gchar* message = g_strdup_printf(
        "simple_query: Linux native operation failed - %s", error.what());
    return constructor("unavailable", message, nullptr);
  } catch (...) {
    return constructor("unavailable",
                       "simple_query: Linux native operation failed", nullptr);
  }
}

/**
 * Purpose: Execute capability discovery behind the C++ exception boundary.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated capability response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiGetCapabilitiesResponse* HandleGetCapabilities(
    gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiGetCapabilitiesResponse>(
      [&] {
        auto result = GetHostApi(user_data)->GetCapabilities();
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_get_capabilities_response_new_error);
        }
        return sqlq_native_query_host_api_get_capabilities_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_get_capabilities_response_new_error);
}

/**
 * Purpose: Execute one query behind the C++ exception boundary.
 * @param request is the decoded Pigeon query request.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated query response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiQueryResponse* HandleQuery(FlValue* request,
                                                 gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiQueryResponse>(
      [&] {
        auto result = GetHostApi(user_data)->Query(request);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_query_response_new_error);
        }
        return sqlq_native_query_host_api_query_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_query_response_new_error);
}

/**
 * Purpose: Execute one mutation behind the C++ exception boundary.
 * @param request is the decoded Pigeon mutation request.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated mutation response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiMutateResponse* HandleMutate(FlValue* request,
                                                   gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiMutateResponse>(
      [&] {
        auto result = GetHostApi(user_data)->Mutate(request);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_mutate_response_new_error);
        }
        return sqlq_native_query_host_api_mutate_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_mutate_response_new_error);
}

/**
 * Purpose: Execute one batch behind the C++ exception boundary.
 * @param request is the decoded Pigeon batch request.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated batch response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiBatchResponse* HandleBatch(FlValue* request,
                                                 gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiBatchResponse>(
      [&] {
        auto result = GetHostApi(user_data)->Batch(request);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_batch_response_new_error);
        }
        return sqlq_native_query_host_api_batch_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_batch_response_new_error);
}

/**
 * Purpose: Start one observer behind the C++ exception boundary.
 * @param request is the decoded Pigeon observation request.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated observer identifier response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiObserveStartResponse* HandleObserveStart(
    FlValue* request, gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiObserveStartResponse>(
      [&] {
        auto result = GetHostApi(user_data)->ObserveStart(request);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_observe_start_response_new_error);
        }
        return sqlq_native_query_host_api_observe_start_response_new(
            result.value.c_str());
      },
      sqlq_native_query_host_api_observe_start_response_new_error);
}

/**
 * Purpose: Stop one observer behind the C++ exception boundary.
 * @param observer_id identifies the observer to stop.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated stop response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiObserveStopResponse* HandleObserveStop(
    const gchar* observer_id, gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiObserveStopResponse>(
      [&] {
        auto error = GetHostApi(user_data)->ObserveStop(observer_id);
        if (error.has_value()) {
          return ErrorResponse(
              *error,
              sqlq_native_query_host_api_observe_stop_response_new_error);
        }
        return sqlq_native_query_host_api_observe_stop_response_new();
      },
      sqlq_native_query_host_api_observe_stop_response_new_error);
}

/**
 * Purpose: Open binary content behind the C++ exception boundary.
 * @param request is the decoded Pigeon binary request.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated binary-handle response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiOpenBinaryResponse* HandleOpenBinary(
    FlValue* request, gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiOpenBinaryResponse>(
      [&] {
        auto result = GetHostApi(user_data)->OpenBinary(request);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_open_binary_response_new_error);
        }
        return sqlq_native_query_host_api_open_binary_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_open_binary_response_new_error);
}

/**
 * Purpose: Close binary content behind the C++ exception boundary.
 * @param handle_id identifies the native handle to close.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated close response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiCloseBinaryResponse* HandleCloseBinary(
    const gchar* handle_id, gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiCloseBinaryResponse>(
      [&] {
        auto error = GetHostApi(user_data)->CloseBinary(handle_id);
        if (error.has_value()) {
          return ErrorResponse(
              *error,
              sqlq_native_query_host_api_close_binary_response_new_error);
        }
        return sqlq_native_query_host_api_close_binary_response_new();
      },
      sqlq_native_query_host_api_close_binary_response_new_error);
}

/**
 * Purpose: Call a Linux extension behind the C++ exception boundary.
 * @param name_space selects the registered extension namespace.
 * @param method selects the extension method.
 * @param args contains optional decoded arguments.
 * @param user_data owns the registered plugin and host implementation.
 * @returns A newly owned generated extension response.
 * @throws Nothing.
 */
SqlqNativeQueryHostApiCallExtensionResponse* HandleCallExtension(
    const gchar* name_space, const gchar* method, FlValue* args,
    gpointer user_data) {
  return GuardHostCallback<SqlqNativeQueryHostApiCallExtensionResponse>(
      [&] {
        auto result =
            GetHostApi(user_data)->CallExtension(name_space, method, args);
        if (result.error.has_value()) {
          return ErrorResponse(
              *result.error,
              sqlq_native_query_host_api_call_extension_response_new_error);
        }
        return sqlq_native_query_host_api_call_extension_response_new(
            result.value.get());
      },
      sqlq_native_query_host_api_call_extension_response_new_error);
}

const SqlqNativeQueryHostApiVTable kHostApiVTable = {
    HandleGetCapabilities, HandleQuery,        HandleMutate,
    HandleBatch,           HandleObserveStart, HandleObserveStop,
    HandleOpenBinary,      HandleCloseBinary,  HandleCallExtension,
};

}  // namespace

/** Purpose: Unregister channels before destroying observers and native state.
 * @param object is the plugin instance being disposed. @returns Nothing.
 * @throws Nothing. */
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

/** Purpose: Register the generated HostApi with registrar-owned transport.
 * @param registrar supplies the messenger and owns plugin integration.
 * @returns Nothing. @throws Nothing across the public C boundary. */
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
