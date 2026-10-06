#pragma once

std::atomic<bool> g_observer_snapshot_failure_logged{false};

/** Purpose: Record the production transient-snapshot warning as readiness.
 * @param domain is the GLib log domain.
 * @param level identifies warning severity.
 * @param message is the production diagnostic.
 * @param user_data is unused test context.
 * @returns Nothing.
 * @throws Nothing. */
void CaptureObserverSnapshotWarning(const gchar* domain, GLogLevelFlags level,
                                    const gchar* message,
                                    gpointer user_data) {
  if (g_strstr_len(message, -1, "snapshot unavailable") != nullptr) {
    g_observer_snapshot_failure_logged.store(true);
  }
}

/** Purpose: Prove an in-place media rewrite changes the observer snapshot.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestMediaObserverDetectsInPlaceUpdate() {
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-media-observer-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  const std::filesystem::path media = root / "photo.jpg";
  std::ofstream(media, std::ios::binary) << "initial";

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "media");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) start_args = RequestArguments(request.release());
  g_autoptr(FlValue) start_response =
      InvokeHost(messenger, "observeStart", start_args);
  const std::string observer_id =
      fl_value_get_string(fl_value_get_list_value(start_response, 0));

  g_usleep(350000);
  const gint64 deadline = g_get_monotonic_time() + 5000000;
  int revision = 0;
  while (messenger->finish_count == 0 &&
         g_get_monotonic_time() < deadline) {
    std::ofstream(media, std::ios::binary | std::ios::trunc)
        << "updated-media-" << revision++;
    RunUntil(context, [&] { return messenger->finish_count > 0; }, 350);
  }
  g_assert_cmpint(messenger->finish_count, >, 0);

  auto stop_args = Value(fl_value_new_list());
  fl_value_append_take(stop_args.get(),
                       fl_value_new_string(observer_id.c_str()));
  g_autoptr(FlValue) stop_response =
      InvokeHost(messenger, "observeStop", stop_args.get());
  ClearHost(messenger);
  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

/** Purpose: Recover observation after a transient filesystem access failure.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestObserverRecoversAfterSnapshotFailure() {
  if (geteuid() == 0) {
    g_test_skip("Permission isolation requires a non-root test process.");
    return;
  }
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-observer-recovery-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  const std::filesystem::path record = root / "record.txt";
  std::ofstream(record) << "initial";
  g_autoptr(GMainContext) context = g_main_context_new();
  g_main_context_push_thread_default(context);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetInt(request.get(), "pollingIntervalMs", 250);
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  g_autoptr(FlValue) start_arguments = RequestArguments(request.release());
  g_autoptr(FlValue) start_response =
      InvokeHost(messenger, "observeStart", start_arguments);
  const std::string observer_id =
      fl_value_get_string(fl_value_get_list_value(start_response, 0));

  g_observer_snapshot_failure_logged.store(false);
  const GLogLevelFlags previous_fatal =
      g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK));
  const guint handler = g_log_set_handler(
      nullptr, G_LOG_LEVEL_WARNING, CaptureObserverSnapshotWarning, nullptr);
  g_assert_cmpint(g_chmod(root.c_str(), 0), ==, 0);
  const gint64 failure_deadline = g_get_monotonic_time() + 3000000;
  while (!g_observer_snapshot_failure_logged.load() &&
         g_get_monotonic_time() < failure_deadline) {
    g_usleep(1000);
  }
  g_assert_true(g_observer_snapshot_failure_logged.load());
  g_log_remove_handler(nullptr, handler);
  g_log_set_always_fatal(previous_fatal);
  g_assert_cmpint(g_chmod(root.c_str(), 0700), ==, 0);

  const gint64 delivery_deadline = g_get_monotonic_time() + 5000000;
  int mutation = 0;
  while (messenger->finish_count == 0 &&
         g_get_monotonic_time() < delivery_deadline) {
    std::ofstream(record, std::ios::trunc) << "restored-" << mutation++;
    RunUntil(context, [&] { return messenger->finish_count == 1; }, 350);
  }
  g_assert_cmpint(messenger->finish_count, ==, 1);

  auto stop_arguments = Value(fl_value_new_list());
  fl_value_append_take(stop_arguments.get(),
                       fl_value_new_string(observer_id.c_str()));
  g_autoptr(FlValue) stop_response =
      InvokeHost(messenger, "observeStop", stop_arguments.get());
  ClearHost(messenger);
  g_main_context_pop_thread_default(context);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}
