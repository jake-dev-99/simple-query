#include <flutter_linux/flutter_linux.h>
#include <glib/gstdio.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

#include "../native_query.g.cc"
#include "../simple_query_linux_plugin.cc"

namespace {

constexpr char kFlutterApiChannel[] =
    "dev.flutter.pigeon.simple_query_linux.NativeQueryFlutterApi."
    "onObserveEvent";
constexpr char kHostApiPrefix[] =
    "dev.flutter.pigeon.simple_query_linux.NativeQueryHostApi.";

enum class SendMode {
  kSuccess,
  kDartError,
  kTransportError,
  kHoldUntilCancelled,
};

struct Handler {
  FlBinaryMessengerMessageHandler callback;
  gpointer user_data;
  GDestroyNotify destroy_notify;
};

struct _TestResponseHandle {
  FlBinaryMessengerResponseHandle parent_instance;
  GBytes* response;
};

struct _TestResponseHandleClass {
  FlBinaryMessengerResponseHandleClass parent_class;
};

using TestResponseHandle = _TestResponseHandle;
using TestResponseHandleClass = _TestResponseHandleClass;

#define TEST_TYPE_RESPONSE_HANDLE (test_response_handle_get_type())
GType test_response_handle_get_type();
G_DEFINE_TYPE(TestResponseHandle, test_response_handle,
              fl_binary_messenger_response_handle_get_type())
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestResponseHandle, g_object_unref)

// Purpose: Release the response bytes captured by a fake response handle.
// Parameters: object is the response handle being disposed.
// Returns: Nothing. Throws: Never.
void test_response_handle_dispose(GObject* object) {
  auto* self = reinterpret_cast<TestResponseHandle*>(object);
  g_clear_pointer(&self->response, g_bytes_unref);
  G_OBJECT_CLASS(test_response_handle_parent_class)->dispose(object);
}

// Purpose: Install lifecycle behavior for the fake response handle type.
// Parameters: klass is the class record initialized by GLib.
// Returns: Nothing. Throws: Never.
void test_response_handle_class_init(TestResponseHandleClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = test_response_handle_dispose;
}

// Purpose: Initialize a fake response handle with no captured response.
// Parameters: self is the newly allocated handle.
// Returns: Nothing. Throws: Never.
void test_response_handle_init(TestResponseHandle* self) {
  self->response = nullptr;
}

struct _TestBinaryMessenger {
  GObject parent_instance;
  GHashTable* handlers;
  FlValue* outgoing_response;
  SendMode send_mode;
  GThread* send_thread;
  gint send_count;
  gint finish_count;
  gint cancellation_count;
};

struct _TestBinaryMessengerClass {
  GObjectClass parent_class;
};

using TestBinaryMessenger = _TestBinaryMessenger;
using TestBinaryMessengerClass = _TestBinaryMessengerClass;

#define TEST_TYPE_BINARY_MESSENGER (test_binary_messenger_get_type())
#define TEST_BINARY_MESSENGER(object) \
  (reinterpret_cast<TestBinaryMessenger*>(object))
GType test_binary_messenger_get_type();

void test_binary_messenger_iface_init(FlBinaryMessengerInterface* interface);
void test_plugin_registrar_iface_init(FlPluginRegistrarInterface* interface);

G_DEFINE_TYPE_WITH_CODE(
    TestBinaryMessenger, test_binary_messenger, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_binary_messenger_get_type(),
                          test_binary_messenger_iface_init)
        G_IMPLEMENT_INTERFACE(fl_plugin_registrar_get_type(),
                              test_plugin_registrar_iface_init))
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TestBinaryMessenger, g_object_unref)

using namespace simple_query_linux;

// Purpose: Release a registered fake channel handler and its user data.
// Parameters: data is the Handler allocated during registration.
// Returns: Nothing. Throws: Never.
void handler_free(gpointer data) {
  auto* handler = static_cast<Handler*>(data);
  if (handler->destroy_notify != nullptr) {
    handler->destroy_notify(handler->user_data);
  }
  g_free(handler);
}

// Purpose: Store or remove a binary channel handler exactly as Flutter does.
// Parameters: messenger owns the registry; channel identifies the handler;
// callback, user_data, and destroy_notify describe its lifetime.
// Returns: Nothing. Throws: Never.
void test_set_message_handler(FlBinaryMessenger* messenger,
                              const gchar* channel,
                              FlBinaryMessengerMessageHandler callback,
                              gpointer user_data,
                              GDestroyNotify destroy_notify) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  if (callback == nullptr) {
    g_hash_table_remove(self->handlers, channel);
    return;
  }
  auto* handler = g_new0(Handler, 1);
  handler->callback = callback;
  handler->user_data = user_data;
  handler->destroy_notify = destroy_notify;
  g_hash_table_replace(self->handlers, g_strdup(channel), handler);
}

// Purpose: Capture the response returned by a registered HostApi handler.
// Parameters: response_handle receives response; response is copied; other
// parameters match the binary messenger interface.
// Returns: TRUE after capture. Throws: Never.
gboolean test_send_response(FlBinaryMessenger* messenger,
                            FlBinaryMessengerResponseHandle* response_handle,
                            GBytes* response, GError** error) {
  auto* handle = reinterpret_cast<TestResponseHandle*>(response_handle);
  g_clear_pointer(&handle->response, g_bytes_unref);
  handle->response = response == nullptr ? nullptr : g_bytes_ref(response);
  return TRUE;
}

struct PendingSend {
  GTask* task;
};

// Purpose: Complete one fake Dart-bound platform message asynchronously.
// Parameters: user_data owns the pending GTask.
// Returns: G_SOURCE_REMOVE after one completion. Throws: Never.
gboolean complete_pending_send(gpointer user_data) {
  auto* pending = static_cast<PendingSend*>(user_data);
  auto* self = TEST_BINARY_MESSENGER(g_task_get_source_object(pending->task));
  if (!g_task_return_error_if_cancelled(pending->task)) {
    if (self->send_mode == SendMode::kTransportError) {
      g_task_return_new_error(pending->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "transport failed");
    } else {
      g_autoptr(FlMessageCodec) codec =
          FL_MESSAGE_CODEC(sqlq_message_codec_new());
      g_autoptr(GError) error = nullptr;
      GBytes* bytes = fl_message_codec_encode_message(
          codec, self->outgoing_response, &error);
      g_assert_no_error(error);
      g_task_return_pointer(
          pending->task, bytes,
          reinterpret_cast<GDestroyNotify>(g_bytes_unref));
    }
  }
  g_object_unref(pending->task);
  g_free(pending);
  return G_SOURCE_REMOVE;
}

// Purpose: Complete a deliberately held fake send only when plugin disposal
// cancels it.
// @param cancellable is the production observer's cancellation token.
// @param user_data owns the pending GTask.
// @returns Nothing.
// @throws Nothing.
void complete_cancelled_send(GCancellable* cancellable, gpointer user_data) {
  auto* pending = static_cast<PendingSend*>(user_data);
  auto* self = TEST_BINARY_MESSENGER(g_task_get_source_object(pending->task));
  self->cancellation_count += 1;
  g_assert_true(g_task_return_error_if_cancelled(pending->task));
  g_object_unref(pending->task);
  g_free(pending);
}

// Purpose: Record and asynchronously answer a message sent to Dart.
// Parameters: messenger and callback follow Flutter's async transport contract;
// channel and message are observed by the fake; cancellable controls completion.
// Returns: Nothing. Throws: Never.
void test_send_on_channel(FlBinaryMessenger* messenger, const gchar* channel,
                          GBytes* message, GCancellable* cancellable,
                          GAsyncReadyCallback callback, gpointer user_data) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  self->send_thread = g_thread_self();
  self->send_count += 1;
  g_assert_cmpstr(channel, ==, kFlutterApiChannel);

  auto* pending = g_new0(PendingSend, 1);
  pending->task = g_task_new(self, cancellable, callback, user_data);
  if (self->send_mode == SendMode::kHoldUntilCancelled) {
    g_assert_nonnull(cancellable);
    g_cancellable_connect(cancellable, G_CALLBACK(complete_cancelled_send),
                          pending, nullptr);
    return;
  }
  GSource* source = g_idle_source_new();
  g_source_set_callback(source, complete_pending_send, pending, nullptr);
  g_source_attach(source, g_main_context_get_thread_default());
  g_source_unref(source);
}

// Purpose: Transfer the fake transport response to Flutter's channel wrapper.
// Parameters: result is the GTask completed by test_send_on_channel; error
// receives cancellation or transport failure.
// Returns: Owned response bytes on success, otherwise nullptr. Throws: Never.
GBytes* test_send_on_channel_finish(FlBinaryMessenger* messenger,
                                    GAsyncResult* result, GError** error) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  self->finish_count += 1;
  return static_cast<GBytes*>(
      g_task_propagate_pointer(G_TASK(result), error));
}

void test_resize_channel(FlBinaryMessenger*, const gchar*, int64_t) {}
void test_set_warns_on_overflow(FlBinaryMessenger*, const gchar*, bool) {}

// Purpose: Model Flutter engine shutdown without mutating a handler table while
// plugin disposal re-enters channel clearing.
// @param messenger owns the current handler registry.
// @returns Nothing.
// @throws Nothing.
void test_shutdown(FlBinaryMessenger* messenger) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  GHashTable* old_handlers = self->handlers;
  self->handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         handler_free);
  g_hash_table_remove_all(old_handlers);
  g_hash_table_unref(old_handlers);
}

// Purpose: Bind the fake's transport behavior to FlBinaryMessenger.
// Parameters: interface is Flutter's binary messenger vtable.
// Returns: Nothing. Throws: Never.
void test_binary_messenger_iface_init(FlBinaryMessengerInterface* interface) {
  interface->set_message_handler_on_channel = test_set_message_handler;
  interface->send_response = test_send_response;
  interface->send_on_channel = test_send_on_channel;
  interface->send_on_channel_finish = test_send_on_channel_finish;
  interface->resize_channel = test_resize_channel;
  interface->set_warns_on_channel_overflow = test_set_warns_on_overflow;
  interface->shutdown = test_shutdown;
}

// Purpose: Return the fake's binary-messenger interface to real registration.
// @param registrar is the dual registrar/messenger fake.
// @returns The registrar-owned binary messenger.
// @throws Nothing.
FlBinaryMessenger* test_registrar_get_messenger(FlPluginRegistrar* registrar) {
  return FL_BINARY_MESSENGER(registrar);
}

// Purpose: Keep texture registration out of this headless native test fixture.
// @param registrar is the dual registrar/messenger fake.
// @returns Null because no texture registrar is needed.
// @throws Nothing.
FlTextureRegistrar* test_registrar_get_texture_registrar(
    FlPluginRegistrar* registrar) {
  return nullptr;
}

// Purpose: Model a headless Flutter registrar for native plugin tests.
// @param registrar is the dual registrar/messenger fake.
// @returns Null because no Flutter view exists.
// @throws Nothing.
FlView* test_registrar_get_view(FlPluginRegistrar* registrar) {
  return nullptr;
}

// Purpose: Bind the fake to Flutter's public registrar contract.
// @param interface is Flutter's plugin-registrar vtable.
// @returns Nothing.
// @throws Nothing.
void test_plugin_registrar_iface_init(FlPluginRegistrarInterface* interface) {
  interface->get_messenger = test_registrar_get_messenger;
  interface->get_texture_registrar = test_registrar_get_texture_registrar;
  interface->get_view = test_registrar_get_view;
}

// Purpose: Release fake handlers and the configured outgoing response.
// Parameters: object is the fake messenger being disposed.
// Returns: Nothing. Throws: Never.
void test_binary_messenger_dispose(GObject* object) {
  auto* self = TEST_BINARY_MESSENGER(object);
  g_clear_pointer(&self->handlers, g_hash_table_unref);
  g_clear_pointer(&self->outgoing_response, fl_value_unref);
  G_OBJECT_CLASS(test_binary_messenger_parent_class)->dispose(object);
}

// Purpose: Install lifecycle behavior for the fake messenger type.
// Parameters: klass is the class record initialized by GLib.
// Returns: Nothing. Throws: Never.
void test_binary_messenger_class_init(TestBinaryMessengerClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = test_binary_messenger_dispose;
}

// Purpose: Initialize deterministic fake transport state.
// Parameters: self is the newly allocated messenger.
// Returns: Nothing. Throws: Never.
void test_binary_messenger_init(TestBinaryMessenger* self) {
  self->handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         handler_free);
  self->outgoing_response = fl_value_new_list();
  fl_value_append_take(self->outgoing_response, fl_value_new_null());
  self->send_mode = SendMode::kSuccess;
  self->send_thread = nullptr;
  self->send_count = 0;
  self->finish_count = 0;
  self->cancellation_count = 0;
}

// Purpose: Create a fake messenger that exercises Flutter's real channel code.
// Parameters: None.
// Returns: A newly owned fake messenger. Throws: Never.
TestBinaryMessenger* NewMessenger() {
  return TEST_BINARY_MESSENGER(
      g_object_new(TEST_TYPE_BINARY_MESSENGER, nullptr));
}

// Purpose: Configure the value or error returned by Dart-bound sends.
// Parameters: self is the fake; mode selects success, Dart error, or transport
// error.
// Returns: Nothing. Throws: Never.
void SetSendMode(TestBinaryMessenger* self, SendMode mode) {
  self->send_mode = mode;
  g_clear_pointer(&self->outgoing_response, fl_value_unref);
  self->outgoing_response = fl_value_new_list();
  if (mode == SendMode::kDartError) {
    fl_value_append_take(self->outgoing_response,
                         fl_value_new_string("dart-error"));
    fl_value_append_take(self->outgoing_response,
                         fl_value_new_string("Dart rejected event"));
    fl_value_append_take(self->outgoing_response, fl_value_new_null());
  } else {
    fl_value_append_take(self->outgoing_response, fl_value_new_null());
  }
}

// Purpose: Drive a GLib context until a behavioral condition becomes true.
// Parameters: context is iterated; condition is polled; timeout bounds waiting.
// Returns: True when the condition succeeds before the deadline. Throws: Never.
bool RunUntil(GMainContext* context, const std::function<bool()>& condition,
              int timeout_ms = 3000) {
  const gint64 deadline =
      g_get_monotonic_time() + static_cast<gint64>(timeout_ms) * 1000;
  while (!condition() && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(context, FALSE)) {
    }
    g_usleep(1000);
  }
  return condition();
}

// Purpose: Invoke a registered Pigeon HostApi channel through real codecs.
// Parameters: self owns handlers; method selects the channel; arguments is the
// Pigeon argument list.
// Returns: A decoded, newly owned Pigeon response. Throws: C++ host exceptions
// until production catches them at the handler boundary.
FlValue* InvokeHost(TestBinaryMessenger* self, const std::string& method,
                    FlValue* arguments) {
  const std::string channel = std::string(kHostApiPrefix) + method;
  auto* handler = static_cast<Handler*>(
      g_hash_table_lookup(self->handlers, channel.c_str()));
  g_assert_nonnull(handler);
  g_autoptr(FlMessageCodec) codec = FL_MESSAGE_CODEC(sqlq_message_codec_new());
  g_autoptr(GError) error = nullptr;
  g_autoptr(GBytes) message =
      fl_message_codec_encode_message(codec, arguments, &error);
  g_assert_no_error(error);
  g_autoptr(TestResponseHandle) handle = reinterpret_cast<TestResponseHandle*>(
      g_object_new(TEST_TYPE_RESPONSE_HANDLE, nullptr));
  handler->callback(FL_BINARY_MESSENGER(self), channel.c_str(), message,
                    FL_BINARY_MESSENGER_RESPONSE_HANDLE(handle),
                    handler->user_data);
  g_assert_nonnull(handle->response);
  FlValue* response =
      fl_message_codec_decode_message(codec, handle->response, &error);
  g_assert_no_error(error);
  return response;
}

// Purpose: Exercise the public plugin registration entrypoint on the fake
// registrar and verify all generated HostApi channels are installed.
// @param self is the dual fake registrar and binary messenger.
// @returns Nothing.
// @throws NativeQueryHostApiImpl construction failures.
void InstallHost(TestBinaryMessenger* self) {
  simple_query_linux_plugin_register_with_registrar(FL_PLUGIN_REGISTRAR(self));
  g_assert_cmpuint(g_hash_table_size(self->handlers), ==, 9);
}

// Purpose: Model engine teardown and prove generated handlers are all removed.
// @param self is the dual fake registrar and binary messenger.
// @returns Nothing.
// @throws Nothing.
void ClearHost(TestBinaryMessenger* self) {
  auto* messenger = FL_BINARY_MESSENGER(self);
  FL_BINARY_MESSENGER_GET_IFACE(messenger)->shutdown(messenger);
  FL_BINARY_MESSENGER_GET_IFACE(messenger)->shutdown(messenger);
  g_assert_cmpuint(g_hash_table_size(self->handlers), ==, 0);
}

// Purpose: Build a Pigeon argument list around one map request.
// Parameters: request is transferred into the argument list.
// Returns: A newly owned argument list. Throws: Never.
FlValue* RequestArguments(FlValue* request) {
  FlValue* args = fl_value_new_list();
  fl_value_append_take(args, request);
  return args;
}

// Purpose: Extract a structured error code from a Pigeon response envelope.
// Parameters: response is the decoded response list.
// Returns: The error code string, or empty for a success envelope. Throws:
// Never.
std::string ResponseErrorCode(FlValue* response) {
  if (fl_value_get_length(response) <= 1) {
    return "";
  }
  return fl_value_get_string(fl_value_get_list_value(response, 0));
}

// Purpose: Extract a structured error message from a Pigeon response envelope.
// Parameters: response is the decoded response list.
// Returns: The error message string, or empty for a success envelope. Throws:
// Never.
std::string ResponseErrorMessage(FlValue* response) {
  if (fl_value_get_length(response) <= 1) {
    return "";
  }
  return fl_value_get_string(fl_value_get_list_value(response, 1));
}

struct AsyncCapture {
  gboolean completed = FALSE;
  SqlqNativeQueryFlutterApiOnObserveEventResponse* response = nullptr;
  GError* error = nullptr;
};

// Purpose: Finish the generated FlutterApi send exactly as plugin code must.
// Parameters: object is the generated API; result is its async result;
// user_data receives the decoded response or transport error.
// Returns: Nothing. Throws: Never.
void CaptureFlutterApiResult(GObject* object, GAsyncResult* result,
                             gpointer user_data) {
  auto* capture = static_cast<AsyncCapture*>(user_data);
  capture->response = sqlq_native_query_flutter_api_on_observe_event_finish(
      SQLQ_NATIVE_QUERY_FLUTTER_API(object), result, &capture->error);
  capture->completed = TRUE;
}

// Purpose: Exercise generated async success, Dart error, transport error, and
// cancellation with the real basic-message-channel stack.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestGeneratedAsyncLifecycle() {
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  g_autoptr(SqlqNativeQueryFlutterApi) api =
      sqlq_native_query_flutter_api_new(FL_BINARY_MESSENGER(messenger),
                                        nullptr);
  g_autoptr(FlValue) event = fl_value_new_map();

  AsyncCapture success;
  sqlq_native_query_flutter_api_on_observe_event(
      api, "observer", event, nullptr, CaptureFlutterApiResult, &success);
  g_assert_true(RunUntil(nullptr, [&] { return success.completed; }));
  g_assert_no_error(success.error);
  g_assert_nonnull(success.response);
  g_assert_false(sqlq_native_query_flutter_api_on_observe_event_response_is_error(
      success.response));
  g_clear_object(&success.response);

  SetSendMode(messenger, SendMode::kDartError);
  AsyncCapture dart_error;
  sqlq_native_query_flutter_api_on_observe_event(
      api, "observer", event, nullptr, CaptureFlutterApiResult, &dart_error);
  g_assert_true(RunUntil(nullptr, [&] { return dart_error.completed; }));
  g_assert_no_error(dart_error.error);
  g_assert_true(sqlq_native_query_flutter_api_on_observe_event_response_is_error(
      dart_error.response));
  g_assert_cmpstr(
      sqlq_native_query_flutter_api_on_observe_event_response_get_error_code(
          dart_error.response),
      ==, "dart-error");
  g_clear_object(&dart_error.response);

  SetSendMode(messenger, SendMode::kTransportError);
  AsyncCapture transport_error;
  sqlq_native_query_flutter_api_on_observe_event(
      api, "observer", event, nullptr, CaptureFlutterApiResult,
      &transport_error);
  g_assert_true(RunUntil(nullptr, [&] { return transport_error.completed; }));
  g_assert_null(transport_error.response);
  g_assert_error(transport_error.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&transport_error.error);

  SetSendMode(messenger, SendMode::kSuccess);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  AsyncCapture cancelled;
  sqlq_native_query_flutter_api_on_observe_event(
      api, "observer", event, cancellable, CaptureFlutterApiResult,
      &cancelled);
  g_cancellable_cancel(cancellable);
  g_assert_true(RunUntil(nullptr, [&] { return cancelled.completed; }));
  g_assert_null(cancelled.response);
  g_assert_error(cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&cancelled.error);
  g_assert_cmpint(messenger->finish_count, ==, 3);
}

// Purpose: Prove a generated async call with no completion callback releases
// its task and source API after the transport completes.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestGeneratedNullCallbackCleanup() {
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  auto* api = sqlq_native_query_flutter_api_new(FL_BINARY_MESSENGER(messenger),
                                                nullptr);
  gpointer weak_api = api;
  g_object_add_weak_pointer(G_OBJECT(api), &weak_api);
  g_autoptr(FlValue) event = fl_value_new_map();
  sqlq_native_query_flutter_api_on_observe_event(
      api, "observer", event, nullptr, nullptr, nullptr);
  g_object_unref(api);
  g_assert_true(RunUntil(nullptr, [&] { return weak_api == nullptr; }));
  g_assert_cmpint(messenger->finish_count, ==, 0);
}

// Purpose: Verify filesystem query failures are stable HostApi errors rather
// than C++ exceptions crossing the generated C callback.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestDanglingSymlinkReturnsUnavailable() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  std::filesystem::create_symlink(root / "missing", root / "dangling");

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  g_autoptr(FlValue) request = fl_value_new_map();
  MapSetString(request, "domain", "files");
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request, "platformData", platform_data.release());
  g_autoptr(FlValue) arguments = RequestArguments(fl_value_ref(request));

  bool threw = false;
  FlValue* raw_response = nullptr;
  try {
    raw_response = InvokeHost(messenger, "query", arguments);
  } catch (...) {
    threw = true;
  }
  g_assert_false(threw);
  g_autoptr(FlValue) response = raw_response;
  g_assert_nonnull(response);
  const std::string error_code = ResponseErrorCode(response);
  const std::string error_message = ResponseErrorMessage(response);
  g_assert_cmpstr(error_code.c_str(), ==, "unavailable");
  g_assert_true(error_message.rfind("simple_query:", 0) == 0);

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

/**
 * Purpose: Prove an unreadable query root is an error, never empty data.
 * @returns Nothing.
 * @throws Nothing.
 */
void TestUnreadableRootReturnsUnavailable() {
  if (geteuid() == 0) {
    g_test_skip("Permission isolation requires a non-root test process.");
    return;
  }
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root = std::filesystem::path(temporary) / "root";
  std::filesystem::create_directory(root);
  g_assert_cmpint(g_chmod(root.c_str(), 0), ==, 0);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  g_autoptr(FlValue) response = InvokeHost(messenger, "query", arguments);
  const std::string code = ResponseErrorCode(response);
  const std::string message = ResponseErrorMessage(response);
  g_assert_cmpstr(code.c_str(), ==, "unavailable");
  g_assert_true(message.rfind("simple_query:", 0) == 0);
  ClearHost(messenger);
  g_assert_cmpint(g_chmod(root.c_str(), 0700), ==, 0);
  std::error_code cleanup_error;
  std::filesystem::remove_all(temporary, cleanup_error);
}

// Purpose: Prove an unexpected C++ filesystem exception is translated at the
// real generated HostApi callback boundary.
// @param None.
// @returns Nothing.
// @throws Nothing.
void TestHostExceptionBoundaryReturnsUnavailable() {
  g_autofree gchar* original_directory = g_get_current_dir();
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-cwd-XXXXXX",
                                               nullptr);
  g_assert_nonnull(temporary);
  g_assert_cmpint(g_chdir(temporary), ==, 0);
  g_assert_cmpint(g_rmdir(temporary), ==, 0);

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto assert_unavailable = [&](const char* method, FlValue* arguments) {
    bool threw = false;
    FlValue* raw_response = nullptr;
    try {
      raw_response = InvokeHost(messenger, method, arguments);
    } catch (...) {
      threw = true;
    }
    g_assert_false(threw);
    g_autoptr(FlValue) response = raw_response;
    g_assert_nonnull(response);
    const std::string code = ResponseErrorCode(response);
    const std::string message = ResponseErrorMessage(response);
    g_assert_cmpstr(code.c_str(), ==, "unavailable");
    g_assert_true(message.rfind("simple_query:", 0) == 0);
  };

  auto query = Value(fl_value_new_map());
  MapSetString(query.get(), "domain", "files");
  g_autoptr(FlValue) query_args = RequestArguments(query.release());
  assert_unavailable("query", query_args);

  auto mutation = Value(fl_value_new_map());
  MapSetString(mutation.get(), "domain", "files");
  MapSetString(mutation.get(), "type", "delete");
  MapSet(mutation.get(), "filters", fl_value_new_list());
  g_autoptr(FlValue) mutation_args = RequestArguments(mutation.release());
  assert_unavailable("mutate", mutation_args);

  auto batch = Value(fl_value_new_map());
  auto operations = Value(fl_value_new_list());
  auto operation = Value(fl_value_new_map());
  MapSetString(operation.get(), "domain", "files");
  MapSetString(operation.get(), "type", "delete");
  MapSet(operation.get(), "filters", fl_value_new_list());
  fl_value_append_take(operations.get(), operation.release());
  MapSet(batch.get(), "operations", operations.release());
  g_autoptr(FlValue) batch_args = RequestArguments(batch.release());
  assert_unavailable("batch", batch_args);

  auto extension_args = Value(fl_value_new_list());
  fl_value_append_take(extension_args.get(), fl_value_new_string("linux.xdg"));
  fl_value_append_take(extension_args.get(),
                       fl_value_new_string("listIndexScopes"));
  fl_value_append_take(extension_args.get(), fl_value_new_null());
  assert_unavailable("callExtension", extension_args.get());

  g_assert_cmpint(g_chdir(original_directory), ==, 0);
  ClearHost(messenger);
}

// Purpose: Exercise query, mutate, batch, and extension boundaries through the
// production HostApi channels and codecs.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestFilesystemAndExtensionBoundaries() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  std::ofstream(root / "existing.txt") << "existing";

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  auto query = Value(fl_value_new_map());
  MapSetString(query.get(), "domain", "files");
  auto query_platform = Value(fl_value_new_map());
  MapSetString(query_platform.get(), "rootPath", root.string());
  MapSet(query.get(), "platformData", query_platform.release());
  g_autoptr(FlValue) query_args = RequestArguments(query.release());
  g_autoptr(FlValue) query_response =
      InvokeHost(messenger, "query", query_args);
  g_assert_cmpuint(fl_value_get_length(query_response), ==, 1);
  FlValue* query_result = fl_value_get_list_value(query_response, 0);
  g_assert_cmpuint(fl_value_get_length(FindValue(query_result, "records")), ==,
                   1);

  auto insert = Value(fl_value_new_map());
  MapSetString(insert.get(), "domain", "files");
  MapSetString(insert.get(), "type", "insert");
  auto insert_values = Value(fl_value_new_map());
  MapSetString(insert_values.get(), "path", (root / "inserted.txt").string());
  MapSetString(insert_values.get(), "content", "created");
  MapSet(insert.get(), "values", insert_values.release());
  g_autoptr(FlValue) insert_args = RequestArguments(insert.release());
  g_autoptr(FlValue) insert_response =
      InvokeHost(messenger, "mutate", insert_args);
  g_assert_cmpuint(fl_value_get_length(insert_response), ==, 1);
  g_assert_true(std::filesystem::exists(root / "inserted.txt"));

  auto batch = Value(fl_value_new_map());
  auto operations = Value(fl_value_new_list());
  for (const char* name : {"batch-a.txt", "batch-b.txt"}) {
    auto operation = Value(fl_value_new_map());
    MapSetString(operation.get(), "domain", "files");
    MapSetString(operation.get(), "type", "insert");
    auto values = Value(fl_value_new_map());
    MapSetString(values.get(), "path", (root / name).string());
    MapSetString(values.get(), "content", name);
    MapSet(operation.get(), "values", values.release());
    fl_value_append_take(operations.get(), operation.release());
  }
  MapSet(batch.get(), "operations", operations.release());
  g_autoptr(FlValue) batch_args = RequestArguments(batch.release());
  g_autoptr(FlValue) batch_response =
      InvokeHost(messenger, "batch", batch_args);
  g_assert_cmpuint(fl_value_get_length(batch_response), ==, 1);
  FlValue* batch_result = fl_value_get_list_value(batch_response, 0);
  g_assert_cmpuint(fl_value_get_length(FindValue(batch_result, "results")), ==,
                   2);

  auto extension_args = Value(fl_value_new_list());
  fl_value_append_take(extension_args.get(), fl_value_new_string("linux.xdg"));
  fl_value_append_take(extension_args.get(), fl_value_new_string("unknown"));
  fl_value_append_take(extension_args.get(), fl_value_new_null());
  g_autoptr(FlValue) extension_response =
      InvokeHost(messenger, "callExtension", extension_args.get());
  const std::string extension_error = ResponseErrorCode(extension_response);
  g_assert_cmpstr(extension_error.c_str(), ==, "not-supported");

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

// Purpose: Prove observer events enter Flutter only on the captured platform
// context and every send is finished.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestObserverDispatchesOnPlatformContext() {
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GThread* platform_thread = g_thread_self();
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root(temporary);

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) start_args = RequestArguments(request.release());
  g_autoptr(FlValue) start_response =
      InvokeHost(messenger, "observeStart", start_args);
  const gchar* observer_id =
      fl_value_get_string(fl_value_get_list_value(start_response, 0));
  g_assert_nonnull(observer_id);

  const gint64 pending_deadline = g_get_monotonic_time() + 5000000;
  int mutation = 0;
  while (!g_main_context_pending(context) &&
         g_get_monotonic_time() < pending_deadline) {
    std::ofstream(root / "changed.txt", std::ios::trunc) << mutation++;
    g_usleep(300000);
  }
  g_assert_true(g_main_context_pending(context));
  g_assert_cmpint(messenger->send_count, ==, 0);
  g_assert_true(RunUntil(context, [&] { return messenger->finish_count == 1; },
                         5000));
  g_assert_true(messenger->send_thread == platform_thread);

  auto stop_args = Value(fl_value_new_list());
  fl_value_append_take(stop_args.get(), fl_value_new_string(observer_id));
  g_autoptr(FlValue) stop_response =
      InvokeHost(messenger, "observeStop", stop_args.get());
  g_assert_cmpuint(fl_value_get_length(stop_response), ==, 1);
  ClearHost(messenger);
  g_main_context_pop_thread_default(context);

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

// Purpose: Verify stopping an observer destroys queued platform sources before
// they can retain or deliver an event.
// Parameters: None. Returns: Nothing. Throws: Never.
void TestObserverStopDropsQueuedDelivery() {
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root(temporary);

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) start_args = RequestArguments(request.release());
  g_autoptr(FlValue) start_response =
      InvokeHost(messenger, "observeStart", start_args);
  const std::string observer_id =
      fl_value_get_string(fl_value_get_list_value(start_response, 0));

  const gint64 pending_deadline = g_get_monotonic_time() + 5000000;
  int mutation = 0;
  while (!g_main_context_pending(context) &&
         g_get_monotonic_time() < pending_deadline) {
    std::ofstream(root / "changed.txt", std::ios::trunc) << mutation++;
    g_usleep(300000);
  }
  g_assert_true(g_main_context_pending(context));
  g_assert_cmpint(messenger->send_count, ==, 0);
  auto stop_args = Value(fl_value_new_list());
  fl_value_append_take(stop_args.get(),
                       fl_value_new_string(observer_id.c_str()));
  g_autoptr(FlValue) stop_response =
      InvokeHost(messenger, "observeStop", stop_args.get());
  while (g_main_context_iteration(context, FALSE)) {
  }
  g_assert_cmpint(messenger->send_count, ==, 0);

  ClearHost(messenger);
  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

// Purpose: Verify Dart and transport delivery failures are completed and
// reported instead of being dropped silently.
// @param None.
// @returns Nothing.
// @throws Nothing.
void TestObserverReportsDeliveryFailures() {
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root(temporary);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  auto run_failure = [&](SendMode mode, const char* expected_warning,
                         int expected_finish_count) {
    SetSendMode(messenger, mode);
    auto request = Value(fl_value_new_map());
    MapSetString(request.get(), "domain", "files");
    MapSetInt(request.get(), "pollingIntervalMs", 250);
    auto platform_data = Value(fl_value_new_map());
    MapSetString(platform_data.get(), "rootPath", root.string());
    MapSet(request.get(), "platformData", platform_data.release());
    g_autoptr(FlValue) start_args = RequestArguments(request.release());
    g_autoptr(FlValue) start_response =
        InvokeHost(messenger, "observeStart", start_args);
    const std::string observer_id =
        fl_value_get_string(fl_value_get_list_value(start_response, 0));

    const gint64 deadline = g_get_monotonic_time() + 5000000;
    int mutation = 0;
    while (!g_main_context_pending(context) &&
           g_get_monotonic_time() < deadline) {
      std::ofstream(root / "failure.txt", std::ios::trunc) << mutation++;
      g_usleep(300000);
    }
    g_assert_true(g_main_context_pending(context));
    g_test_expect_message(nullptr, G_LOG_LEVEL_WARNING, expected_warning);
    g_assert_true(RunUntil(
        context,
        [&] { return messenger->finish_count == expected_finish_count; }));
    g_test_assert_expected_messages();

    auto stop_args = Value(fl_value_new_list());
    fl_value_append_take(stop_args.get(),
                         fl_value_new_string(observer_id.c_str()));
    g_autoptr(FlValue) stop_response =
        InvokeHost(messenger, "observeStop", stop_args.get());
  };

  run_failure(SendMode::kDartError, "*delivery rejected (dart-error)*", 1);
  run_failure(SendMode::kTransportError,
              "*delivery failed - transport failed*", 2);
  ClearHost(messenger);
  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

// Purpose: Verify real plugin disposal cancels an in-flight observer send and
// the generated completion still finishes safely afterward.
// @param None.
// @returns Nothing.
// @throws Nothing.
void TestObserverDisposalCancelsInFlightDelivery() {
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root(temporary);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  SetSendMode(messenger, SendMode::kHoldUntilCancelled);
  InstallHost(messenger);

  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) args = RequestArguments(request.release());
  g_autoptr(FlValue) response = InvokeHost(messenger, "observeStart", args);

  const gint64 deadline = g_get_monotonic_time() + 5000000;
  int mutation = 0;
  while (messenger->send_count == 0 && g_get_monotonic_time() < deadline) {
    std::ofstream(root / "cancel.txt", std::ios::trunc) << mutation++;
    RunUntil(context, [&] { return messenger->send_count == 1; }, 350);
  }
  g_assert_cmpint(messenger->send_count, ==, 1);
  g_assert_cmpint(messenger->finish_count, ==, 0);
  ClearHost(messenger);
  g_assert_cmpint(messenger->cancellation_count, ==, 1);
  g_assert_true(RunUntil(
      context, [&] { return G_OBJECT(messenger)->ref_count == 1; }));

  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

// Purpose: Register and run Linux native behavioral regression tests.
// Parameters: argc and argv are GLib test runner arguments.
// Returns: The GLib test process exit status. Throws: Never.
int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/simple_query/generated/async_lifecycle",
                  TestGeneratedAsyncLifecycle);
  g_test_add_func("/simple_query/generated/null_callback_cleanup",
                  TestGeneratedNullCallbackCleanup);
  g_test_add_func("/simple_query/host/dangling_symlink",
                  TestDanglingSymlinkReturnsUnavailable);
  g_test_add_func("/simple_query/host/unreadable_root",
                  TestUnreadableRootReturnsUnavailable);
  g_test_add_func("/simple_query/host/exception_boundary",
                  TestHostExceptionBoundaryReturnsUnavailable);
  g_test_add_func("/simple_query/host/filesystem_and_extension",
                  TestFilesystemAndExtensionBoundaries);
  g_test_add_func("/simple_query/observer/platform_context",
                  TestObserverDispatchesOnPlatformContext);
  g_test_add_func("/simple_query/observer/stop_drops_queued",
                  TestObserverStopDropsQueuedDelivery);
  g_test_add_func("/simple_query/observer/reports_delivery_failures",
                  TestObserverReportsDeliveryFailures);
  g_test_add_func("/simple_query/observer/disposal_cancels_in_flight",
                  TestObserverDisposalCancelsInFlightDelivery);
  return g_test_run();
}
