#pragma once

/** Purpose: Verify generated finish results without hiding asynchronous errors.
 * @param completed records whether the real finish callback ran.
 * @param response owns the generated reply until the test releases it.
 * @param error owns a transport failure until the test releases it.
 * @returns Capture state for success, rejection, and cancellation assertions.
 * @throws Nothing. */
struct AsyncCapture {
  gboolean completed = FALSE;
  SqlqNativeQueryFlutterApiOnObserveEventResponse* response = nullptr;
  GError* error = nullptr;
};

/** Purpose: Finish a generated FlutterApi send. @param object is its API.
 * @param result is async output. @param user_data receives it.
 * @returns Nothing. @throws Nothing. */
void CaptureFlutterApiResult(GObject* object, GAsyncResult* result,
                             gpointer user_data) {
  auto* capture = static_cast<AsyncCapture*>(user_data);
  capture->response = sqlq_native_query_flutter_api_on_observe_event_finish(
      SQLQ_NATIVE_QUERY_FLUTTER_API(object), result, &capture->error);
  capture->completed = TRUE;
}

/** Purpose: Exercise generated async success, errors, and cancellation.
 * @param None. @returns Nothing. @throws Nothing. */
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
  g_assert_cmpint(messenger->finish_count, ==, 4);
}

/** Purpose: Prove null-callback async sends release task and source API.
 * @param None. @returns Nothing. @throws Nothing. */
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
  g_assert_cmpint(messenger->finish_count, ==, 1);
  g_assert_cmpint(G_OBJECT(messenger)->ref_count, ==, 1);
}
