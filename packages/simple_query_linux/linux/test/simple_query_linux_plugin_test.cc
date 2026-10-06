#include <flutter_linux/flutter_linux.h>
#include <glib/gstdio.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

#include "../native_query.g.cc"

gpointer g_observed_cancellable = nullptr;

/** Purpose: Expose cancellable lifetime through a GLib weak pointer.
 * @returns A newly owned cancellable. @throws Nothing. */
GCancellable* NewObservedCancellable() {
  auto* cancellable = g_cancellable_new();
  g_observed_cancellable = cancellable;
  g_object_add_weak_pointer(G_OBJECT(cancellable), &g_observed_cancellable);
  return cancellable;
}

#define g_cancellable_new NewObservedCancellable
#include "../simple_query_linux_plugin.cc"
#undef g_cancellable_new

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

/** Purpose: Release captured response bytes. @param object is the fake handle.
 * @returns Nothing. @throws Nothing. */
void test_response_handle_dispose(GObject* object) {
  auto* self = reinterpret_cast<TestResponseHandle*>(object);
  g_clear_pointer(&self->response, g_bytes_unref);
  G_OBJECT_CLASS(test_response_handle_parent_class)->dispose(object);
}

/** Purpose: Install fake response disposal. @param klass is the GLib class.
 * @returns Nothing. @throws Nothing. */
void test_response_handle_class_init(TestResponseHandleClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = test_response_handle_dispose;
}

/** Purpose: Initialize empty response capture. @param self is the new handle.
 * @returns Nothing. @throws Nothing. */
void test_response_handle_init(TestResponseHandle* self) {
  self->response = nullptr;
}

struct _TestBinaryMessenger {
  GObject parent_instance;
  GHashTable* handlers;
  FlValue* outgoing_response;
  GBytes* last_outgoing_message;
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

/** Purpose: Release a registered fake handler. @param data owns the handler.
 * @returns Nothing. @throws Nothing. */
void handler_free(gpointer data) {
  auto* handler = static_cast<Handler*>(data);
  if (handler->destroy_notify != nullptr) {
    handler->destroy_notify(handler->user_data);
  }
  g_free(handler);
}

/** Purpose: Store or remove a channel handler with Flutter lifecycle rules.
 * @param messenger owns registrations. @param channel names the handler.
 * @param callback handles messages. @param user_data is callback context.
 * @param destroy_notify releases context. @returns Nothing. @throws Nothing. */
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

/** Purpose: Capture a real HostApi response. @param response_handle receives
 * bytes. @param response is copied. @returns True. @throws Nothing. */
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

/** Purpose: Complete one Dart-bound send asynchronously. @param user_data owns
 * its task. @returns G_SOURCE_REMOVE. @throws Nothing. */
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

/** Purpose: Complete a held send only after disposal cancellation.
 * @param cancellable is the observer token. @param user_data owns its task.
 * @returns Nothing. @throws Nothing. */
void complete_cancelled_send(GCancellable* cancellable, gpointer user_data) {
  auto* pending = static_cast<PendingSend*>(user_data);
  auto* self = TEST_BINARY_MESSENGER(g_task_get_source_object(pending->task));
  self->cancellation_count += 1;
  g_assert_true(g_task_return_error_if_cancelled(pending->task));
  g_object_unref(pending->task);
  g_free(pending);
}

/** Purpose: Record and asynchronously answer one Dart-bound message.
 * @param messenger is the fake transport. @param channel names the API.
 * @param message is retained for replay. @param cancellable controls completion.
 * @param callback receives completion. @param user_data is callback context.
 * @returns Nothing. @throws Nothing. */
void test_send_on_channel(FlBinaryMessenger* messenger, const gchar* channel,
                          GBytes* message, GCancellable* cancellable,
                          GAsyncReadyCallback callback, gpointer user_data) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  self->send_thread = g_thread_self();
  self->send_count += 1;
  g_assert_cmpstr(channel, ==, kFlutterApiChannel);
  g_clear_pointer(&self->last_outgoing_message, g_bytes_unref);
  self->last_outgoing_message = g_bytes_ref(message);

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

/** Purpose: Transfer the fake transport result to Flutter's channel wrapper.
 * @param result is the completed task. @param error receives failure.
 * @returns Owned bytes or null. @throws Nothing. */
GBytes* test_send_on_channel_finish(FlBinaryMessenger* messenger,
                                    GAsyncResult* result, GError** error) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  self->finish_count += 1;
  return static_cast<GBytes*>(
      g_task_propagate_pointer(G_TASK(result), error));
}

void test_resize_channel(FlBinaryMessenger*, const gchar*, int64_t) {}
void test_set_warns_on_overflow(FlBinaryMessenger*, const gchar*, bool) {}

/** Purpose: Model engine shutdown while plugin disposal clears handlers.
 * @param messenger owns the registry. @returns Nothing. @throws Nothing. */
void test_shutdown(FlBinaryMessenger* messenger) {
  auto* self = TEST_BINARY_MESSENGER(messenger);
  GHashTable* old_handlers = self->handlers;
  self->handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         handler_free);
  g_hash_table_remove_all(old_handlers);
  g_hash_table_unref(old_handlers);
}

/** Purpose: Bind fake transport behavior. @param interface is its vtable.
 * @returns Nothing. @throws Nothing. */
void test_binary_messenger_iface_init(FlBinaryMessengerInterface* interface) {
  interface->set_message_handler_on_channel = test_set_message_handler;
  interface->send_response = test_send_response;
  interface->send_on_channel = test_send_on_channel;
  interface->send_on_channel_finish = test_send_on_channel_finish;
  interface->resize_channel = test_resize_channel;
  interface->set_warns_on_channel_overflow = test_set_warns_on_overflow;
  interface->shutdown = test_shutdown;
}

/** Purpose: Expose the fake messenger through the registrar contract.
 * @param registrar is the dual fake. @returns Its borrowed messenger.
 * @throws Nothing. */
FlBinaryMessenger* test_registrar_get_messenger(FlPluginRegistrar* registrar) {
  return FL_BINARY_MESSENGER(registrar);
}

/** Purpose: Keep texture registration out of the headless fixture.
 * @param registrar is the dual fake. @returns Null. @throws Nothing. */
FlTextureRegistrar* test_registrar_get_texture_registrar(
    FlPluginRegistrar* registrar) {
  return nullptr;
}

/** Purpose: Model a headless registrar view. @param registrar is the dual fake.
 * @returns Null. @throws Nothing. */
FlView* test_registrar_get_view(FlPluginRegistrar* registrar) {
  return nullptr;
}

/** Purpose: Bind the fake registrar contract. @param interface is its vtable.
 * @returns Nothing. @throws Nothing. */
void test_plugin_registrar_iface_init(FlPluginRegistrarInterface* interface) {
  interface->get_messenger = test_registrar_get_messenger;
  interface->get_texture_registrar = test_registrar_get_texture_registrar;
  interface->get_view = test_registrar_get_view;
}

/** Purpose: Release fake transport resources. @param object is the messenger.
 * @returns Nothing. @throws Nothing. */
void test_binary_messenger_dispose(GObject* object) {
  auto* self = TEST_BINARY_MESSENGER(object);
  g_clear_pointer(&self->handlers, g_hash_table_unref);
  g_clear_pointer(&self->outgoing_response, fl_value_unref);
  g_clear_pointer(&self->last_outgoing_message, g_bytes_unref);
  G_OBJECT_CLASS(test_binary_messenger_parent_class)->dispose(object);
}

/** Purpose: Install fake messenger disposal. @param klass is the GLib class.
 * @returns Nothing. @throws Nothing. */
void test_binary_messenger_class_init(TestBinaryMessengerClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = test_binary_messenger_dispose;
}

/** Purpose: Initialize deterministic transport state. @param self is new.
 * @returns Nothing. @throws Nothing. */
void test_binary_messenger_init(TestBinaryMessenger* self) {
  self->handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         handler_free);
  self->outgoing_response = fl_value_new_list();
  fl_value_append_take(self->outgoing_response, fl_value_new_null());
  self->last_outgoing_message = nullptr;
  self->send_mode = SendMode::kSuccess;
  self->send_thread = nullptr;
  self->send_count = 0;
  self->finish_count = 0;
  self->cancellation_count = 0;
}

/** Purpose: Create transport that exercises Flutter's real channels.
 * @returns A newly owned fake messenger. @throws Nothing. */
TestBinaryMessenger* NewMessenger() {
  return TEST_BINARY_MESSENGER(
      g_object_new(TEST_TYPE_BINARY_MESSENGER, nullptr));
}

/** Purpose: Configure Dart-bound send results. @param self is the fake.
 * @param mode selects success or failure. @returns Nothing. @throws Nothing. */
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

/** Purpose: Drive a GLib context until a condition or deadline.
 * @param context is iterated. @param condition is polled. @param timeout_ms
 * bounds waiting. @returns True on success. @throws Nothing. */
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

/** Purpose: Invoke a real registered HostApi channel and codecs. @param self
 * owns handlers. @param method selects the channel. @param arguments is the
 * Pigeon list. @param encoded_response optionally captures wire bytes.
 * @returns A decoded owned response. @throws Escaped host exceptions. */
FlValue* InvokeHost(TestBinaryMessenger* self, const std::string& method,
                    FlValue* arguments, GBytes** encoded_response = nullptr) {
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
  if (encoded_response != nullptr) {
    *encoded_response = g_bytes_ref(handle->response);
  }
  FlValue* response =
      fl_message_codec_decode_message(codec, handle->response, &error);
  g_assert_no_error(error);
  return response;
}

/**
 * Purpose: Persist real native codec bytes for the Dart boundary test.
 * @param name selects the fixture filename under the configured directory.
 * @param bytes contains one generated Pigeon message or response.
 * @returns Nothing.
 * @throws Nothing.
 */
void WriteContractFixture(const char* name, GBytes* bytes) {
  const gchar* directory = g_getenv("SIMPLE_QUERY_CONTRACT_FIXTURE_DIR");
  if (directory == nullptr) return;
  g_autofree gchar* path = g_build_filename(directory, name, nullptr);
  gsize length = 0;
  const auto* data = static_cast<const gchar*>(g_bytes_get_data(bytes, &length));
  g_autoptr(GError) error = nullptr;
  g_assert_true(g_file_set_contents(path, data, length, &error));
  g_assert_no_error(error);
}

/** Purpose: Exercise public registration and all HostApi channel installs.
 * @param self is the dual fake. @returns Nothing. @throws Host construction
 * failures. */
void InstallHost(TestBinaryMessenger* self) {
  simple_query_linux_plugin_register_with_registrar(FL_PLUGIN_REGISTRAR(self));
  g_assert_cmpuint(g_hash_table_size(self->handlers), ==, 9);
}

/** Purpose: Model engine teardown and verify handler removal. @param self is
 * the dual fake. @returns Nothing. @throws Nothing. */
void ClearHost(TestBinaryMessenger* self) {
  auto* messenger = FL_BINARY_MESSENGER(self);
  FL_BINARY_MESSENGER_GET_IFACE(messenger)->shutdown(messenger);
  FL_BINARY_MESSENGER_GET_IFACE(messenger)->shutdown(messenger);
  g_assert_cmpuint(g_hash_table_size(self->handlers), ==, 0);
}

/** Purpose: Wrap one request in Pigeon arguments. @param request transfers.
 * @returns A newly owned list. @throws Nothing. */
FlValue* RequestArguments(FlValue* request) {
  FlValue* args = fl_value_new_list();
  fl_value_append_take(args, request);
  return args;
}

/**
 * Purpose: Append one insert operation for ordered native batch tests.
 * @param operations receives the new mutation map.
 * @param domain identifies the mutation domain.
 * @param path is null for an intentionally malformed insert.
 * @param is_directory requests directory creation instead of a file.
 * @returns Nothing.
 * @throws Nothing.
 */
void AppendInsertOperation(FlValue* operations, const char* domain,
                           const std::filesystem::path* path,
                           bool is_directory = false) {
  auto operation = Value(fl_value_new_map());
  MapSetString(operation.get(), "domain", domain);
  MapSetString(operation.get(), "type", "insert");
  auto values = Value(fl_value_new_map());
  if (path != nullptr) {
    MapSetString(values.get(), "path", path->string());
    MapSetString(values.get(), "content", "created");
    MapSetBool(values.get(), "isDirectory", is_directory);
  }
  MapSet(operation.get(), "values", values.release());
  fl_value_append_take(operations, operation.release());
}

/** Purpose: Read a response error code. @param response is the decoded list.
 * @returns Its code or empty on success. @throws Nothing. */
std::string ResponseErrorCode(FlValue* response) {
  if (fl_value_get_length(response) <= 1) {
    return "";
  }
  return fl_value_get_string(fl_value_get_list_value(response, 0));
}

/** Purpose: Read a response error message. @param response is decoded.
 * @returns Its message or empty on success. @throws Nothing. */
std::string ResponseErrorMessage(FlValue* response) {
  if (fl_value_get_length(response) <= 1) {
    return "";
  }
  return fl_value_get_string(fl_value_get_list_value(response, 1));
}

#include "generated_flutter_api_lifecycle_test.h"
#include "host_failure_test.h"

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

/**
 * Purpose: Prove update and delete permission failures are never false success.
 * @returns Nothing.
 * @throws Nothing.
 */
void TestMutationFilesystemErrorsReturnUnavailable() {
  if (geteuid() == 0) {
    g_test_skip("Permission isolation requires a non-root test process.");
    return;
  }
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-test-XXXXXX",
                                               nullptr);
  const std::filesystem::path root(temporary);
  const std::filesystem::path locked = root / "locked";
  const std::filesystem::path file = locked / "record.txt";
  std::filesystem::create_directory(locked);
  std::ofstream(file) << "before";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  g_assert_cmpint(g_chmod(locked.c_str(), 0), ==, 0);
  auto update = Value(fl_value_new_map());
  MapSetString(update.get(), "domain", "files");
  MapSetString(update.get(), "type", "update");
  auto values = Value(fl_value_new_map());
  MapSetString(values.get(), "path", file.string());
  MapSetString(values.get(), "content", "after");
  MapSet(update.get(), "values", values.release());
  g_autoptr(FlValue) update_args = RequestArguments(update.release());
  g_autoptr(FlValue) update_response =
      InvokeHost(messenger, "mutate", update_args);
  const std::string update_error = ResponseErrorCode(update_response);
  g_assert_cmpstr(update_error.c_str(), ==, "unavailable");

  g_assert_cmpint(g_chmod(locked.c_str(), 0500), ==, 0);
  auto removal = Value(fl_value_new_map());
  MapSetString(removal.get(), "domain", "files");
  MapSetString(removal.get(), "type", "delete");
  auto filter = Value(fl_value_new_map());
  MapSetString(filter.get(), "field", "path");
  MapSetString(filter.get(), "operator", "equals");
  MapSetString(filter.get(), "value", file.string());
  auto filters = Value(fl_value_new_list());
  fl_value_append_take(filters.get(), filter.release());
  MapSet(removal.get(), "filters", filters.release());
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(removal.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) removal_args = RequestArguments(removal.release());
  g_autoptr(FlValue) removal_response =
      InvokeHost(messenger, "mutate", removal_args);
  const std::string removal_error = ResponseErrorCode(removal_response);
  g_assert_cmpstr(removal_error.c_str(), ==, "unavailable");

  ClearHost(messenger);
  g_assert_cmpint(g_chmod(locked.c_str(), 0700), ==, 0);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

/** Purpose: Prove unexpected filesystem exceptions become HostApi errors.
 * @returns Nothing. @throws Nothing. */
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
  g_autoptr(FlValue) batch_response =
      InvokeHost(messenger, "batch", batch_args);
  FlValue* batch_payload = fl_value_get_list_value(batch_response, 0);
  FlValue* batch_results = FindValue(batch_payload, "results");
  g_assert_cmpuint(fl_value_get_length(batch_results), ==, 1);
  FlValue* failed_result = fl_value_get_list_value(batch_results, 0);
  FlValue* error = FindValue(FindValue(failed_result, "metadata"), "error");
  g_assert_cmpstr(fl_value_get_string(FindValue(error, "code")), ==,
                  "unavailable");
  g_assert_cmpstr(fl_value_get_string(FindValue(error, "domain")), ==,
                  "files");
  g_assert_cmpstr(fl_value_get_string(FindValue(error, "operation")), ==,
                  "write");

  auto extension_args = Value(fl_value_new_list());
  fl_value_append_take(extension_args.get(), fl_value_new_string("linux.xdg"));
  fl_value_append_take(extension_args.get(),
                       fl_value_new_string("listIndexScopes"));
  fl_value_append_take(extension_args.get(), fl_value_new_null());
  assert_unavailable("callExtension", extension_args.get());

  g_assert_cmpint(g_chdir(original_directory), ==, 0);
  ClearHost(messenger);
}

/** Purpose: Exercise query, mutate, batch, and extension channels and codecs.
 * @returns Nothing. @throws Nothing. */
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
  const std::filesystem::path successful_path = root / "batch-success.txt";
  const std::filesystem::path blocker = root / "batch-blocker";
  const std::filesystem::path failed_path = blocker / "nested";
  std::ofstream(blocker) << "not a directory";
  AppendInsertOperation(operations.get(), "files", &failed_path, true);
  AppendInsertOperation(operations.get(), "files", &successful_path);
  AppendInsertOperation(operations.get(), "files", nullptr);
  MapSet(batch.get(), "operations", operations.release());
  g_autoptr(FlValue) batch_args = RequestArguments(batch.release());
  g_autoptr(GBytes) encoded_batch_response = nullptr;
  g_autoptr(FlValue) batch_response =
      InvokeHost(messenger, "batch", batch_args, &encoded_batch_response);
  g_assert_cmpuint(fl_value_get_length(batch_response), ==, 1);
  FlValue* batch_result = fl_value_get_list_value(batch_response, 0);
  FlValue* native_results = FindValue(batch_result, "results");
  g_assert_cmpuint(fl_value_get_length(native_results), ==, 3);
  const std::array<const char*, 3> expected_codes = {"unavailable", nullptr,
                                                     "invalidQuery"};
  for (const size_t index : {0u, 2u}) {
    FlValue* failed = fl_value_get_list_value(native_results, index);
    FlValue* error = FindValue(FindValue(failed, "metadata"), "error");
    g_assert_nonnull(error);
    g_assert_cmpstr(fl_value_get_string(FindValue(error, "code")), ==,
                    expected_codes[index]);
  }
  g_assert_true(std::filesystem::exists(successful_path));
  WriteContractFixture("batch_response.bin", encoded_batch_response);

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

/** Purpose: Prove observer sends use the platform context and always finish.
 * @returns Nothing. @throws Nothing. */
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
  g_assert_nonnull(messenger->last_outgoing_message);
  WriteContractFixture("observe_event.bin",
                       messenger->last_outgoing_message);

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

/** Purpose: Verify stopping an observer destroys queued platform sources.
 * @returns Nothing. @throws Nothing. */
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

/** Purpose: Verify Dart and transport delivery failures stay observable.
 * @returns Nothing. @throws Nothing. */
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

/** Purpose: Verify disposal cancels and safely finishes an in-flight send.
 * @returns Nothing. @throws Nothing. */
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

/** Purpose: Register and run native regressions. @param argc is argument count.
 * @param argv supplies arguments. @returns GLib exit status. @throws Nothing. */
int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/simple_query/generated/async_lifecycle",
                  TestGeneratedAsyncLifecycle);
  g_test_add_func("/simple_query/generated/null_callback_cleanup",
                  TestGeneratedNullCallbackCleanup);
  g_test_add_func("/simple_query/host/dangling_symlink",
                  TestDanglingSymlinkReturnsUnavailable);
  g_test_add_func("/simple_query/host/observer_startup_rollback",
                  TestObserverThreadStartupRollback);
  g_test_add_func("/simple_query/host/unreadable_root",
                  TestUnreadableRootReturnsUnavailable);
  g_test_add_func("/simple_query/host/mutation_filesystem_errors",
                  TestMutationFilesystemErrorsReturnUnavailable);
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
