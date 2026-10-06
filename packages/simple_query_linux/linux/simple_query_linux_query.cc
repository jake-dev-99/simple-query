#include <gtk/gtk.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "simple_query_linux_plugin_private.h"
#ifdef HAS_LIBEBOOK
#include <libebook/libebook.h>
#endif
#ifdef HAS_LIBECAL
#include <libecal/libecal.h>
#endif

namespace simple_query_linux {

/** Purpose: Infer the stable MIME value exposed for a filesystem record.
 * @param path supplies the filename extension.
 * @returns A known MIME type or application/octet-stream.
 * @throws std::bad_alloc on string allocation. */
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

namespace {

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

/** Purpose: Describe one address-book or calendar source discovered via EDS. */
struct EdsSource {
  std::string uid;
  std::string display_name;
  bool is_address_book = false;
  bool is_calendar = false;
};

/** Purpose: Carry discovered EDS sources or their stable source failure. */
struct EdsSourcesResult {
  std::vector<EdsSource> sources;
  std::optional<std::string> error;
};

/** Purpose: Carry portable domain rows or the source failure that blocked them.
 */
struct DomainRowsResult {
  Rows rows;
  std::optional<std::string> error;
};

/** Purpose: Discover EDS sources through the session-bus object manager.
 * @param cancellable optionally interrupts a stalled service call.
 * @returns Source descriptors or a stable unavailable diagnostic.
 * @throws std::bad_alloc when result allocation fails. */
EdsSourcesResult QueryEdsSources(GCancellable* cancellable) {
  EdsSourcesResult result;
  GError* error = nullptr;

  GDBusConnection* connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, &error);
  if (connection == nullptr) {
    result.error =
        std::string("simple_query: EDS service unavailable - ") +
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
      G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1,
      cancellable, &error);

  if (reply == nullptr) {
    result.error =
        std::string("simple_query: EDS service unavailable - ") +
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

#ifdef HAS_LIBEBOOK
/** Purpose: Copy one repeated EDS contact attribute into a Flutter list.
 * @param contact supplies the native contact. @param field selects attributes.
 * @returns A newly owned list of non-empty values. @throws Nothing. */
ValuePtr ContactValues(EContact* contact, EContactField field) {
  auto values = Value(fl_value_new_list());
  GList* attributes = e_contact_get_attributes(contact, field);
  for (GList* link = attributes; link != nullptr; link = link->next) {
    auto* attribute = static_cast<EVCardAttribute*>(link->data);
    gchar* value = e_vcard_attribute_get_value(attribute);
    if (value != nullptr && *value != '\0') {
      fl_value_append_take(values.get(), fl_value_new_string(value));
    }
    g_free(value);
  }
  g_list_free_full(attributes,
                   reinterpret_cast<GDestroyNotify>(e_vcard_attribute_free));
  return values;
}

}  // namespace

/** Purpose: Convert one EDS contact to the portable record schema.
 * @param contact is the borrowed native contact. @returns A newly owned row.
 * @throws Nothing. */
ValuePtr ProjectContactRecord(EContact* contact) {
  auto row = Value(fl_value_new_map());
  const gchar* uid = reinterpret_cast<const gchar*>(
      e_contact_get_const(contact, E_CONTACT_UID));
  const gchar* full_name = reinterpret_cast<const gchar*>(
      e_contact_get_const(contact, E_CONTACT_FULL_NAME));
  const gchar* organization = reinterpret_cast<const gchar*>(
      e_contact_get_const(contact, E_CONTACT_ORG));
  const gchar* revision = reinterpret_cast<const gchar*>(
      e_contact_get_const(contact, E_CONTACT_REV));
  MapSetString(row.get(), "id", uid != nullptr ? uid : "");
  MapSetString(row.get(), "displayName", full_name != nullptr ? full_name : "");
  MapSet(row.get(), "phones", ContactValues(contact, E_CONTACT_TEL).release());
  MapSet(row.get(), "emails",
         ContactValues(contact, E_CONTACT_EMAIL).release());
  MapSet(row.get(), "organization",
         organization != nullptr ? fl_value_new_string(organization)
                                 : fl_value_new_null());
  MapSet(row.get(), "updatedAt",
         revision != nullptr ? fl_value_new_string(revision)
                             : fl_value_new_null());
  return row;
}

namespace {

/** Purpose: Read contact rows through the linked libebook client.
 * @param cancellable optionally interrupts registry and record operations.
 * @returns Contact rows or a stable registry failure.
 * @throws std::bad_alloc when result allocation fails. */
DomainRowsResult ListLibebookContactRecords(GCancellable* cancellable) {
  DomainRowsResult result;
  GError* error = nullptr;
  ESourceRegistry* registry =
      e_source_registry_new_sync(cancellable, &error);
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
        e_book_client_connect_sync(source, 30, cancellable, &error));
    if (client == nullptr) {
      if (error) g_error_free(error);
      error = nullptr;
      continue;
    }
    GSList* contacts = nullptr;
    if (e_book_client_get_contacts_sync(client, "", &contacts, cancellable,
                                        &error)) {
      for (GSList* item = contacts; item != nullptr; item = item->next) {
        EContact* contact = E_CONTACT(item->data);
        result.rows.push_back(ProjectContactRecord(contact));
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
  return result;
}
#endif

#ifndef HAS_LIBEBOOK
/** Purpose: Project EDS discovery into contact rows without libebook.
 * @param cancellable optionally interrupts source discovery.
 * @returns Contact rows or the discovery failure.
 * @throws std::bad_alloc when result allocation fails. */
DomainRowsResult ListDiscoveredContactRecords(GCancellable* cancellable) {
  DomainRowsResult result;
  const auto sources = QueryEdsSources(cancellable);
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
  return result;
}
#endif

/** Purpose: Read portable contact rows from libebook or EDS discovery.
 * @param cancellable optionally interrupts provider work.
 * @returns Contact rows or a stable source error. @throws std::bad_alloc when
 * result allocation fails. */
DomainRowsResult ListContactRecords(GCancellable* cancellable) {
#ifdef HAS_LIBEBOOK
  return ListLibebookContactRecords(cancellable);
#else
  return ListDiscoveredContactRecords(cancellable);
#endif
}

#ifdef HAS_LIBECAL
}  // namespace

/** Purpose: Read an event's modification time without confusing recurrence ID.
 * @param component is the borrowed native event.
 * @returns A newly referenced LAST-MODIFIED value, then DTSTAMP, or null.
 * @throws Nothing. */
ICalTime* CalendarModificationTime(ICalComponent* component) {
  ICalProperty* property = i_cal_component_get_first_property(
      component, I_CAL_LASTMODIFIED_PROPERTY);
  if (property != nullptr) {
    ICalTime* modified = i_cal_property_get_lastmodified(property);
    g_object_unref(property);
    if (modified != nullptr) return modified;
  }
  return i_cal_component_get_dtstamp(component);
}

/** Purpose: Convert one VEVENT component to the portable calendar schema.
 * @param component is the borrowed native event. @param calendar_uid identifies
 * its source calendar. @returns A newly owned row. @throws Nothing. */
ValuePtr ProjectCalendarRecord(ICalComponent* component,
                               const gchar* calendar_uid) {
  auto row = Value(fl_value_new_map());
  const gchar* uid = i_cal_component_get_uid(component);
  const gchar* summary = i_cal_component_get_summary(component);
  ICalTime* start = i_cal_component_get_dtstart(component);
  ICalTime* end = i_cal_component_get_dtend(component);
  MapSetString(row.get(), "id", uid != nullptr ? uid : "");
  MapSetString(row.get(), "title", summary != nullptr ? summary : "");
  if (start != nullptr) {
    gchar* value = i_cal_time_as_ical_string(start);
    MapSetString(row.get(), "startAt", value != nullptr ? value : "");
    MapSetBool(row.get(), "isAllDay", i_cal_time_is_date(start) != 0);
    g_free(value);
    g_object_unref(start);
  } else {
    MapSetString(row.get(), "startAt", "");
    MapSetBool(row.get(), "isAllDay", false);
  }
  if (end != nullptr) {
    gchar* value = i_cal_time_as_ical_string(end);
    MapSetString(row.get(), "endAt", value != nullptr ? value : "");
    g_free(value);
    g_object_unref(end);
  } else {
    MapSetString(row.get(), "endAt", "");
  }
  MapSetString(row.get(), "calendarId",
               calendar_uid != nullptr ? calendar_uid : "");
  ICalTime* modified = CalendarModificationTime(component);
  if (modified != nullptr) {
    gchar* value = i_cal_time_as_ical_string(modified);
    MapSet(row.get(), "updatedAt",
           value != nullptr ? fl_value_new_string(value) : fl_value_new_null());
    g_free(value);
    g_object_unref(modified);
  } else {
    MapSet(row.get(), "updatedAt", fl_value_new_null());
  }
  return row;
}

namespace {

/** Purpose: Append one EDS calendar source's VEVENT records.
 * @param source is the borrowed calendar source. @param expression bounds time.
 * @param result receives portable rows. @param cancellable interrupts reads.
 * @returns Nothing. @throws Nothing. */
void AppendCalendarSource(ESource* source, const gchar* expression,
                          DomainRowsResult* result,
                          GCancellable* cancellable) {
  GError* error = nullptr;
  ECalClient* client = reinterpret_cast<ECalClient*>(e_cal_client_connect_sync(
      source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, cancellable, &error));
  if (client == nullptr) {
    if (error) g_error_free(error);
    return;
  }
  GSList* components = nullptr;
  if (e_cal_client_get_object_list_sync(client, expression, &components,
                                        cancellable, &error)) {
    const gchar* calendar_uid = e_source_get_uid(source);
    for (GSList* item = components; item != nullptr; item = item->next) {
      ICalComponent* component = I_CAL_COMPONENT(item->data);
      if (i_cal_component_isa(component) == I_CAL_VEVENT_COMPONENT) {
        result->rows.push_back(ProjectCalendarRecord(component, calendar_uid));
      }
      g_object_unref(component);
    }
    g_slist_free(components);
  } else if (error) {
    g_error_free(error);
  }
  g_object_unref(client);
}

/** Purpose: Read calendar rows through the linked libecal client.
 * @param cancellable optionally interrupts registry and record operations.
 * @returns Calendar rows or a stable registry failure.
 * @throws std::bad_alloc when result allocation fails. */
DomainRowsResult ListLibecalCalendarRecords(GCancellable* cancellable) {
  DomainRowsResult result;
  GError* error = nullptr;
  ESourceRegistry* registry =
      e_source_registry_new_sync(cancellable, &error);
  if (registry == nullptr) {
    result.error = std::string("simple_query: EDS registry unavailable - ") +
                   (error ? error->message : "unknown");
    if (error) g_error_free(error);
    return result;
  }
  GList* sources =
      e_source_registry_list_sources(registry, E_SOURCE_EXTENSION_CALENDAR);
  const time_t now = time(nullptr);
  ICalTime* start =
      i_cal_time_new_from_timet_with_zone(now - 31536000, 0, nullptr);
  ICalTime* end =
      i_cal_time_new_from_timet_with_zone(now + 31536000, 0, nullptr);
  gchar* start_text = i_cal_time_as_ical_string(start);
  gchar* end_text = i_cal_time_as_ical_string(end);
  gchar* expression = g_strdup_printf(
      "(occur-in-time-range? (make-time \"%s\") (make-time \"%s\"))",
      start_text, end_text);
  for (GList* link = sources; link != nullptr; link = link->next) {
    AppendCalendarSource(E_SOURCE(link->data), expression, &result,
                         cancellable);
  }
  g_free(expression);
  g_free(start_text);
  g_free(end_text);
  g_object_unref(start);
  g_object_unref(end);
  g_list_free_full(sources, g_object_unref);
  g_object_unref(registry);
  return result;
}
#endif

#ifndef HAS_LIBECAL
/** Purpose: Project EDS discovery into calendar rows without libecal.
 * @param cancellable optionally interrupts source discovery.
 * @returns Calendar rows or the discovery failure.
 * @throws std::bad_alloc when result allocation fails. */
DomainRowsResult ListDiscoveredCalendarRecords(GCancellable* cancellable) {
  DomainRowsResult result;
  const auto sources = QueryEdsSources(cancellable);
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
  return result;
}
#endif

/** Purpose: Read portable event rows from libecal or EDS discovery.
 * @param cancellable optionally interrupts provider work.
 * @returns Calendar rows or a stable source error. @throws std::bad_alloc when
 * result allocation fails. */
DomainRowsResult ListCalendarRecords(GCancellable* cancellable) {
#ifdef HAS_LIBECAL
  return ListLibecalCalendarRecords(cancellable);
#else
  return ListDiscoveredCalendarRecords(cancellable);
#endif
}

/** Purpose: Filter discovered EDS sources for one extension API.
 * @param address_books selects address books instead of calendars.
 * @returns Matching sources or the discovery error. @throws std::bad_alloc
 * when result allocation fails. */
EdsSourcesResult ListEdsDomainSources(bool address_books) {
  EdsSourcesResult result;
  const auto sources = QueryEdsSources(nullptr);
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

/** Purpose: Append one portable filesystem record with strict metadata errors.
 * @param entry is the borrowed directory entry. @param media_only selects the
 * media schema. @param result receives a row or failure. @returns True when
 * iteration may continue. @throws Nothing. */
bool AppendFilesystemRecord(const std::filesystem::directory_entry& entry,
                            bool media_only, DomainRowsResult* result) {
  std::error_code error;
  const auto path = entry.path();
  const bool is_directory = entry.is_directory(error);
  if (error) {
    result->error = FileSystemFailure("filesystem metadata", path, error);
    return false;
  }
  const std::string mime = MimeFromPath(path);
  if (media_only && (is_directory || !IsMediaMime(mime))) {
    return true;
  }
  auto row = Value(fl_value_new_map());
  const bool is_regular = entry.is_regular_file(error);
  if (error) {
    result->error = FileSystemFailure("filesystem metadata", path, error);
    return false;
  }
  if (is_regular) {
    const auto size = entry.file_size(error);
    if (error) {
      result->error = FileSystemFailure("filesystem size", path, error);
      return false;
    }
    MapSetInt(row.get(), "size", static_cast<int64_t>(size));
  }
  const auto modified = ModifiedEpochMs(entry, error);
  if (error) {
    result->error = FileSystemFailure("filesystem timestamp", path, error);
    return false;
  }
  MapSetString(row.get(), "id", path.string());
  if (media_only) {
    MapSetString(row.get(), "uriOrPath", path.string());
    MapSetString(row.get(), "mediaType", MediaType(mime));
    MapSetString(row.get(), "mimeType", mime);
    MapSetString(row.get(), "createdAt", std::to_string(modified));
    MapSetString(row.get(), "modifiedAt", std::to_string(modified));
  } else {
    MapSetString(row.get(), "path", path.string());
    MapSetString(row.get(), "name", path.filename().string());
    MapSetBool(row.get(), "isDirectory", is_directory);
    MapSetString(row.get(), "modifiedAt", std::to_string(modified));
    MapSetString(row.get(), "mimeType", mime);
    MapSetString(row.get(), "type", is_directory ? "directory" : "file");
    MapSetString(row.get(), "extension", path.extension().string());
    MapSetInt(row.get(), "modifiedEpochMs", modified);
  }
  result->rows.push_back(std::move(row));
  return true;
}

/** Purpose: Enumerate files without converting access failures into empty data.
 * @param root is the filesystem root selected by the caller.
 * @param media_only filters results to supported media MIME types.
 * @param cancellable optionally interrupts recursive enumeration.
 * @returns Rows on success or a stable simple_query error on inspection
 * failure.
 * @throws Nothing. */
DomainRowsResult ListRecords(const std::filesystem::path& root,
                             bool media_only, GCancellable* cancellable) {
  DomainRowsResult result;
  if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) {
    result.error = "simple_query: filesystem query was cancelled";
    return result;
  }
  std::error_code error;
  const bool root_exists = std::filesystem::exists(root, error);
  if (error) {
    result.error = FileSystemFailure("filesystem query", root, error);
    return result;
  }
  if (!root_exists) return result;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::none, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    result.error = FileSystemFailure("filesystem query", root, error);
    return result;
  }
  while (iterator != end) {
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) {
      result.error = "simple_query: filesystem query was cancelled";
      return result;
    }
    if (!AppendFilesystemRecord(*iterator, media_only, &result)) {
      return result;
    }
    iterator.increment(error);
    if (error) {
      result.error = FileSystemFailure("filesystem query", root, error);
      return result;
    }
  }
  return result;
}

}  // namespace

/** Purpose: Encode every projected field into a collision-free change key.
 * @param row is the borrowed record being snapshotted.
 * @returns Exact StandardMessageCodec bytes or a stable encoding error.
 * @throws std::bad_alloc when copying the encoded bytes fails. */
StringResult SnapshotSignature(FlValue* row) {
  g_autoptr(FlStandardMessageCodec) codec = fl_standard_message_codec_new();
  g_autoptr(GError) error = nullptr;
  g_autoptr(GBytes) encoded =
      fl_message_codec_encode_message(FL_MESSAGE_CODEC(codec), row, &error);
  if (encoded == nullptr) {
    return StringResult{
        "", NativeError{"unavailable",
                        std::string("simple_query: could not encode observer "
                                    "snapshot - ") +
                            (error != nullptr ? error->message
                                              : "encoding failed")}};
  }
  gsize length = 0;
  const auto* bytes =
      static_cast<const char*>(g_bytes_get_data(encoded, &length));
  return StringResult{std::string(bytes, length), std::nullopt};
}

/**
 * Purpose: Build an observer snapshot while preserving data-source failures.
 * @param request is the observer's independent request value.
 * @param domain selects the native record source.
 * @param cancellable optionally interrupts native record acquisition.
 * @returns A snapshot or a stable simple_query error; errors are never treated
 * as an empty snapshot.
 * @throws Nothing.
 */
SnapshotResult BuildSnapshotForDomain(FlValue* request,
                                      const std::string& domain,
                                      GCancellable* cancellable) {
  SnapshotResult result;
  DomainRowsResult records;
  if (domain == "files" || domain == "media") {
    records =
        ListRecords(ResolveRootPath(request), domain == "media", cancellable);
  } else if (domain == "contacts") {
    records = ListContactRecords(cancellable);
  } else if (domain == "calendar") {
    records = ListCalendarRecords(cancellable);
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
    auto signature = SnapshotSignature(row.get());
    if (signature.error.has_value()) {
      result.error = signature.error->message;
      return result;
    }
    result.snapshot[*id] = std::move(signature.value);
  }
  return result;
}

/** Purpose: Describe currently reachable Linux domains and extensions.
 * @returns A portable capability snapshot. @throws std::bad_alloc when the
 * snapshot cannot be allocated. */
ValueResult NativeQueryHostApiImpl::GetCapabilities() {
  const auto eds_probe = QueryEdsSources(nullptr);

  auto capabilities = Value(fl_value_new_list());
  fl_value_append_take(
      capabilities.get(),
      Capability("contacts", !eds_probe.error.has_value(), false,
                 !eds_probe.error.has_value(), false, eds_probe.error)
          .release());
  fl_value_append_take(capabilities.get(),
                       Capability("media", true, true, true, true).release());
  fl_value_append_take(capabilities.get(),
                       Capability("files", true, true, true, true).release());
  fl_value_append_take(
      capabilities.get(),
      Capability("calendar", !eds_probe.error.has_value(), false,
                 !eds_probe.error.has_value(), false, eds_probe.error)
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
ValueResult NativeQueryHostApiImpl::Query(FlValue* request) {
  const std::string domain = StringOr(request, "domain", "platformSpecific");
  if (domain != "files" && domain != "media" && domain != "contacts" &&
      domain != "calendar") {
    return Failure("not-supported",
                   "simple_query: query is not supported for domain " + domain +
                       " on Linux host");
  }

  Rows rows;
  if (domain == "files" || domain == "media") {
    auto file_rows =
        ListRecords(ResolveRootPath(request), domain == "media", nullptr);
    if (file_rows.error.has_value()) {
      return Failure("unavailable", *file_rows.error);
    }
    rows = std::move(file_rows.rows);
  } else if (domain == "contacts") {
    auto contact_rows = ListContactRecords(nullptr);
    if (contact_rows.error.has_value()) {
      return Failure("unavailable", *contact_rows.error);
    }
    rows = std::move(contact_rows.rows);
  } else {
    auto calendar_rows = ListCalendarRecords(nullptr);
    if (calendar_rows.error.has_value()) {
      return Failure("unavailable", *calendar_rows.error);
    }
    rows = std::move(calendar_rows.rows);
  }

  rows = ApplyFilters(std::move(rows), AsList(FindValue(request, "filters")));
  ApplySort(&rows, AsList(FindValue(request, "sort")));

  const int64_t total_count = static_cast<int64_t>(rows.size());
  FlValue* page = AsMap(FindValue(request, "page"));
  const int64_t offset = std::max<int64_t>(
      0, page == nullptr ? 0 : AsInt(FindValue(page, "offset")).value_or(0));
  const auto limit = page == nullptr ? std::optional<int64_t>{}
                                     : AsInt(FindValue(page, "limit"));
  const size_t start =
      static_cast<size_t>(std::min<int64_t>(offset, rows.size()));
  size_t end = rows.size();
  if (limit.has_value()) {
    end = std::min(rows.size(),
                   start + static_cast<size_t>(std::max<int64_t>(0, *limit)));
  }

  Rows paged_rows;
  for (size_t index = start; index < end; index++) {
    paged_rows.push_back(std::move(rows[index]));
  }

  auto result = Value(fl_value_new_map());
  MapSet(result.get(), "records",
         ApplyProjection(paged_rows, AsList(FindValue(request, "projection")))
             .release());
  MapSetInt(result.get(), "totalCount", total_count);
  if (end < rows.size()) {
    MapSetInt(result.get(), "nextOffset", static_cast<int64_t>(end));
  }
  return Success(std::move(result));
}

namespace {

/** Purpose: Encode one EDS discovery extension response.
 * @param address_books selects contacts rather than calendars.
 * @returns Extension data or a structured source error. @throws Nothing. */
ValueResult EdsExtensionResult(bool address_books) {
  const auto sources = ListEdsDomainSources(address_books);
  if (sources.error.has_value()) {
    return Failure("unavailable", *sources.error);
  }
  auto rows = Value(fl_value_new_list());
  for (const auto& source : sources.sources) {
    auto row = Value(fl_value_new_map());
    MapSetString(row.get(), "id", source.uid);
    MapSetString(row.get(), address_books ? "name" : "title",
                 source.display_name);
    fl_value_append_take(rows.get(), row.release());
  }
  auto response = Value(fl_value_new_map());
  MapSet(response.get(), address_books ? "addressBooks" : "calendars",
         rows.release());
  return Success(std::move(response));
}

/** Purpose: Validate that an extension accepts no arguments.
 * @param args is the nullable argument map. @param method names diagnostics.
 * @returns Null on success or a structured invalid-query result.
 * @throws Nothing. */
std::optional<ValueResult> RejectExtensionArguments(FlValue* args,
                                                    const char* method) {
  if (args == nullptr || fl_value_get_length(args) == 0) {
    return std::nullopt;
  }
  return Failure("invalid-query", std::string("simple_query: ") + method +
                                      " does not accept arguments");
}

/** Purpose: Encode Tracker index scopes with an optional limit.
 * @param args is the nullable argument map. @returns Scope data or validation
 * failure. @throws Nothing. */
ValueResult TrackerScopes(FlValue* args) {
  int64_t limit = -1;
  if (args != nullptr) {
    if (FlValue* value = FindValue(args, "limit"); value != nullptr) {
      const auto parsed = AsInt(value);
      if (!parsed.has_value()) {
        return Failure(
            "invalid-query",
            "simple_query: linux.tracker.listIndexScopes expects limit as int");
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
      fl_value_append_take(limited.get(), fl_value_ref(fl_value_get_list_value(
                                              scopes.get(), index)));
    }
    scopes = std::move(limited);
  }
  auto response = Value(fl_value_new_map());
  MapSet(response.get(), "scopes", scopes.release());
  return Success(std::move(response));
}

/** Purpose: Encode the stable Tracker graph list.
 * @returns Graph data. @throws Nothing. */
ValueResult TrackerGraphs() {
  auto graphs = Value(fl_value_new_list());
  for (const char* graph : {"tracker:Documents", "tracker:Pictures",
                            "tracker:Audio", "tracker:Video"}) {
    fl_value_append_take(graphs.get(), fl_value_new_string(graph));
  }
  auto response = Value(fl_value_new_map());
  MapSet(response.get(), "graphs", graphs.release());
  return Success(std::move(response));
}

/** Purpose: Encode XDG search scopes with optional temporary storage.
 * @param args is the nullable argument map. @returns Scope data or validation
 * failure. @throws Filesystem exceptions for the callback guard to translate.
 */
ValueResult XdgScopes(FlValue* args) {
  bool include_temp = true;
  if (args != nullptr) {
    if (FlValue* value = FindValue(args, "includeTemp"); value != nullptr) {
      if (fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
        return Failure(
            "invalid-query",
            "simple_query: linux.xdg.listIndexScopes expects includeTemp "
            "as bool");
      }
      include_temp = fl_value_get_bool(value);
    }
  }
  auto scopes = Value(fl_value_new_list());
  fl_value_append_take(
      scopes.get(),
      fl_value_new_string(std::filesystem::current_path().string().c_str()));
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

}  // namespace

/** Purpose: Dispatch a namespaced Linux diagnostic extension.
 * @param name_space selects the extension. @param method selects its action.
 * @param args contains optional decoded arguments. @returns Extension data or
 * a structured error. @throws Native allocation exceptions for the outer
 * callback boundary to translate. */
ValueResult NativeQueryHostApiImpl::CallExtension(const std::string& name_space,
                                                  const std::string& method,
                                                  FlValue* args) {
  args = AsMap(args);
  if (name_space == "linux.eds" && method == "listAddressBooks") {
    if (auto error =
            RejectExtensionArguments(args, "linux.eds.listAddressBooks")) {
      return std::move(*error);
    }
    return EdsExtensionResult(true);
  }
  if (name_space == "linux.eds" && method == "listCalendars") {
    if (auto error =
            RejectExtensionArguments(args, "linux.eds.listCalendars")) {
      return std::move(*error);
    }
    return EdsExtensionResult(false);
  }
  if (name_space == "linux.tracker" && method == "listIndexScopes") {
    return TrackerScopes(args);
  }
  if (name_space == "linux.tracker" && method == "listGraphNames") {
    if (auto error =
            RejectExtensionArguments(args, "linux.tracker.listGraphNames")) {
      return std::move(*error);
    }
    return TrackerGraphs();
  }
  if (name_space == "linux.xdg" && method == "listIndexScopes") {
    return XdgScopes(args);
  }
  return Failure("not-supported", "simple_query: " + name_space + "." + method +
                                      " is not supported on Linux host");
}

}  // namespace simple_query_linux
