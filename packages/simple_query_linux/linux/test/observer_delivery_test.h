#pragma once

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

    g_test_expect_message(nullptr, G_LOG_LEVEL_WARNING, expected_warning);
    const gint64 deadline = g_get_monotonic_time() + 5000000;
    int mutation = 0;
    while (messenger->finish_count < expected_finish_count &&
           g_get_monotonic_time() < deadline) {
      std::ofstream(root / "failure.txt", std::ios::trunc) << mutation++;
      RunUntil(context,
               [&] {
                 return messenger->finish_count == expected_finish_count;
               },
               350);
    }
    g_assert_cmpint(messenger->finish_count, ==, expected_finish_count);
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
  g_assert_cmpint(messenger->finish_count, ==, 1);
  g_assert_null(g_observed_cancellable);

  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}
