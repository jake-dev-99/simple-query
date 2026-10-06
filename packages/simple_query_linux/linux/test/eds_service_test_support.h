#pragma once

#include <atomic>

enum class EdsManagerCallMode {
  kPassThrough,
  kReturnEmpty,
  kReturnEmptyThenBlock,
};

std::atomic<EdsManagerCallMode> g_eds_manager_call_mode{
    EdsManagerCallMode::kPassThrough};
std::atomic<int> g_eds_manager_call_count{0};
std::atomic<bool> g_eds_manager_call_blocked{false};

/**
 * Purpose: Build an empty ObjectManager result for transport-bound tests.
 * @param None. @returns A newly owned GetManagedObjects reply.
 * @throws Nothing.
 */
GVariant* EmptyManagedObjectsReply() {
  GVariantBuilder objects;
  g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
  return g_variant_ref_sink(g_variant_new("(a{oa{sa{sv}}})", &objects));
}

extern "C" GVariant* __real_g_dbus_connection_call_sync(
    GDBusConnection* connection, const gchar* bus_name,
    const gchar* object_path, const gchar* interface_name,
    const gchar* method_name, GVariant* parameters,
    const GVariantType* reply_type, GDBusCallFlags flags, gint timeout_msec,
    GCancellable* cancellable, GError** error);

/**
 * Purpose: Count real EDS discovery calls and model a cancellable stalled call.
 * @param connection is the real session-bus connection.
 * @param bus_name identifies the destination service.
 * @param object_path identifies the destination object.
 * @param interface_name identifies the D-Bus interface.
 * @param method_name identifies the invoked method.
 * @param parameters carries method arguments.
 * @param reply_type constrains the response type.
 * @param flags configures the D-Bus call.
 * @param timeout_msec bounds the real call.
 * @param cancellable interrupts the stalled test transport.
 * @param error receives cancellation or transport failure.
 * @returns A synthetic EDS reply in test mode or the real transport result.
 * @throws Nothing.
 */
extern "C" GVariant* __wrap_g_dbus_connection_call_sync(
    GDBusConnection* connection, const gchar* bus_name,
    const gchar* object_path, const gchar* interface_name,
    const gchar* method_name, GVariant* parameters,
    const GVariantType* reply_type, GDBusCallFlags flags, gint timeout_msec,
    GCancellable* cancellable, GError** error) {
  const bool is_eds_discovery =
      g_strcmp0(bus_name, "org.gnome.evolution.dataserver.Sources5") == 0 &&
      g_strcmp0(method_name, "GetManagedObjects") == 0;
  const auto mode = g_eds_manager_call_mode.load();
  if (!is_eds_discovery || mode == EdsManagerCallMode::kPassThrough) {
    return __real_g_dbus_connection_call_sync(
        connection, bus_name, object_path, interface_name, method_name,
        parameters, reply_type, flags, timeout_msec, cancellable, error);
  }
  const int call_number = ++g_eds_manager_call_count;
  if (mode == EdsManagerCallMode::kReturnEmpty || call_number == 1) {
    return EmptyManagedObjectsReply();
  }
  g_eds_manager_call_blocked.store(true);
  while (cancellable == nullptr || !g_cancellable_is_cancelled(cancellable)) {
    g_usleep(1000);
  }
  g_cancellable_set_error_if_cancelled(cancellable, error);
  return nullptr;
}

#if defined(HAS_LIBEBOOK) || defined(HAS_LIBECAL)
std::atomic<int> g_eds_registry_construction_count{0};

extern "C" ESourceRegistry* __real_e_source_registry_new_sync(
    GCancellable* cancellable, GError** error);

/**
 * Purpose: Detect expensive EDS client construction during capability probes.
 * @param cancellable interrupts registry creation.
 * @param error receives a registry failure.
 * @returns The real newly owned EDS registry.
 * @throws Nothing.
 */
extern "C" ESourceRegistry* __wrap_e_source_registry_new_sync(
    GCancellable* cancellable, GError** error) {
  ++g_eds_registry_construction_count;
  return __real_e_source_registry_new_sync(cancellable, error);
}
#endif
