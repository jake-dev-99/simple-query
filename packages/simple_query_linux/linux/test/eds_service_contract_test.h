#pragma once

/**
 * Purpose: Exercise one capability probe against a real private session bus.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
void RunCapabilityProbeContract() {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_eds_manager_call_count.store(0);
  g_eds_manager_call_mode.store(EdsManagerCallMode::kReturnEmpty);
#if defined(HAS_LIBEBOOK) || defined(HAS_LIBECAL)
  g_eds_registry_construction_count.store(0);
#endif

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  g_autoptr(FlValue) arguments = fl_value_new_list();
  g_autoptr(FlValue) response =
      InvokeHost(messenger, "getCapabilities", arguments);
  const std::string capability_error = ResponseErrorCode(response);
  g_assert_cmpstr(capability_error.c_str(), ==, "");
  g_assert_cmpint(g_eds_manager_call_count.load(), ==, 1);
#if defined(HAS_LIBEBOOK) || defined(HAS_LIBECAL)
  g_assert_cmpint(g_eds_registry_construction_count.load(), ==, 0);
#endif
  ClearHost(messenger);
  g_eds_manager_call_mode.store(EdsManagerCallMode::kPassThrough);
  g_test_dbus_down(bus);
}

/**
 * Purpose: Prove capabilities probe availability without loading EDS records.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
void TestCapabilitiesUseOneLightweightEdsProbe() {
  if (g_test_subprocess()) {
    RunCapabilityProbeContract();
    return;
  }
  g_test_trap_subprocess(nullptr, 10000000, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed();
}

#if !defined(HAS_LIBEBOOK) && !defined(HAS_LIBECAL)
/**
 * Purpose: Exercise cancellation of a stalled observer discovery call.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
void RunObserverCancellationContract() {
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_eds_manager_call_count.store(0);
  g_eds_manager_call_blocked.store(false);
  g_eds_manager_call_mode.store(EdsManagerCallMode::kReturnEmptyThenBlock);
  g_assert_null(g_observed_cancellable);

  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "contacts");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  g_autoptr(FlValue) start_arguments = RequestArguments(request.release());
  g_autoptr(FlValue) start_response =
      InvokeHost(messenger, "observeStart", start_arguments);
  const gchar* observer_id =
      fl_value_get_string(fl_value_get_list_value(start_response, 0));
  g_assert_nonnull(observer_id);

  const gint64 blocked_deadline = g_get_monotonic_time() + 3000000;
  while (!g_eds_manager_call_blocked.load() &&
         g_get_monotonic_time() < blocked_deadline) {
    g_usleep(1000);
  }
  g_assert_true(g_eds_manager_call_blocked.load());
  auto stop_arguments = Value(fl_value_new_list());
  fl_value_append_take(stop_arguments.get(), fl_value_new_string(observer_id));
  const gint64 stop_started = g_get_monotonic_time();
  g_autoptr(FlValue) stop_response =
      InvokeHost(messenger, "observeStop", stop_arguments.get());
  const gint64 stop_elapsed = g_get_monotonic_time() - stop_started;
  g_assert_cmpint(stop_elapsed, <, 500000);
  const std::string stop_error = ResponseErrorCode(stop_response);
  g_assert_cmpstr(stop_error.c_str(), ==, "");
  g_assert_true(
      RunUntil(context, [] { return g_observed_cancellable == nullptr; }));
  g_assert_cmpint(messenger->send_count, ==, 0);

  ClearHost(messenger);
  while (g_main_context_iteration(context, FALSE)) {
  }
  g_assert_cmpint(messenger->send_count, ==, 0);
  g_main_context_pop_thread_default(context);
  g_eds_manager_call_mode.store(EdsManagerCallMode::kPassThrough);
  g_test_dbus_down(bus);
}

/**
 * Purpose: Prove observeStop cancels blocked discovery without joining worker.
 * @param None. @returns Nothing.
 * @throws Nothing.
 */
void TestObserverStopDoesNotBlockOnDiscovery() {
  if (g_test_subprocess()) {
    RunObserverCancellationContract();
    return;
  }
  g_test_trap_subprocess(nullptr, 10000000, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed();
}
#endif
