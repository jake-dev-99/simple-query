#include "include/simple_query_linux/simple_query_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <exception>
#include <string>
#include <utility>

#include "native_query.g.h"
#include "simple_query_linux_plugin_private.h"

#define SIMPLE_QUERY_LINUX_PLUGIN(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), simple_query_linux_plugin_get_type(), \
                              SimpleQueryLinuxPlugin))

/** Purpose: Tie host lifetime to registrar and generated callback ownership.
 * @param parent_instance supplies the GLib object base.
 * @param registrar retains the registrar until plugin disposal.
 * @param host_api owns the native host released during disposal.
 * @returns A plugin instance with platform-thread-owned transport resources.
 * @throws Nothing; construction uses GLib object allocation. */
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
Response* ErrorResponse(const simple_query_linux::NativeError& error,
                        Response* (*constructor)(const gchar*, const gchar*,
                                                 FlValue*)) {
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
Response* GuardHostCallback(Callback&& callback,
                            Response* (*constructor)(const gchar*, const gchar*,
                                                     FlValue*)) noexcept {
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
SqlqNativeQueryHostApiOpenBinaryResponse* HandleOpenBinary(FlValue* request,
                                                           gpointer user_data) {
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

/** Purpose: Install disposal for the Linux plugin GObject class.
 * @param klass is the GLib class being initialized.
 * @returns Nothing. @throws Nothing. */
static void simple_query_linux_plugin_class_init(
    SimpleQueryLinuxPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = simple_query_linux_plugin_dispose;
}

/** Purpose: Initialize a plugin before registrar-owned resources are attached.
 * @param self is the newly allocated plugin instance.
 * @returns Nothing. @throws Nothing. */
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
  plugin->host_api = new simple_query_linux::NativeQueryHostApiImpl(messenger);
  sqlq_native_query_host_api_set_method_handlers(
      messenger, nullptr, &kHostApiVTable, g_object_ref(plugin),
      g_object_unref);
  g_object_unref(plugin);
}
