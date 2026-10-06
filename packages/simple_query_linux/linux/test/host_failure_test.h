#pragma once

/** Purpose: Verify dangling-link failures become stable HostApi errors.
 * @returns Nothing. @throws Nothing. */
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
 * Purpose: Prove failed native thread startup rolls back observer ownership.
 * @returns Nothing.
 * @throws Nothing.
 */
void TestObserverThreadStartupRollback() {
  if (geteuid() == 0) {
    g_test_skip("Thread resource isolation requires a non-root test process.");
    return;
  }
  g_assert_null(g_observed_cancellable);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  struct rlimit original_limit {};
  g_assert_cmpint(getrlimit(RLIMIT_NPROC, &original_limit), ==, 0);
  struct rlimit startup_limit = original_limit;
  startup_limit.rlim_cur = 1;
  g_assert_cmpint(setrlimit(RLIMIT_NPROC, &startup_limit), ==, 0);
  g_autoptr(FlValue) response =
      InvokeHost(messenger, "observeStart", arguments);
  g_assert_cmpint(setrlimit(RLIMIT_NPROC, &original_limit), ==, 0);

  const std::string error_code = ResponseErrorCode(response);
  const std::string error_message = ResponseErrorMessage(response);
  g_assert_cmpstr(error_code.c_str(), ==, "unavailable");
  g_assert_true(error_message.rfind("simple_query:", 0) == 0);
  g_assert_null(g_observed_cancellable);
  ClearHost(messenger);
}

/**
 * Purpose: Reject an unreadable binary without publishing a native handle.
 * @returns Nothing.
 * @throws Nothing.
 */
void TestUnreadableBinaryReturnsUnavailable() {
  if (geteuid() == 0) {
    g_test_skip("Permission isolation requires a non-root test process.");
    return;
  }
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-test-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  const std::filesystem::path locked_directory = root / "locked";
  std::filesystem::create_directory(locked_directory);
  const std::filesystem::path binary_path = locked_directory / "payload.bin";
  std::ofstream(binary_path, std::ios::binary) << "payload";
  g_assert_cmpint(g_chmod(locked_directory.c_str(), 0), ==, 0);

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetString(request.get(), "recordId", binary_path.string());
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  g_autoptr(FlValue) response = InvokeHost(messenger, "openBinary", arguments);
  g_assert_cmpint(g_chmod(locked_directory.c_str(), 0700), ==, 0);

  const std::string error_code = ResponseErrorCode(response);
  const std::string error_message = ResponseErrorMessage(response);
  g_assert_cmpstr(error_code.c_str(), ==, "unavailable");
  g_assert_nonnull(
      g_strstr_len(error_message.c_str(), -1, "binary existence check"));
  g_assert_nonnull(
      g_strstr_len(error_message.c_str(), -1, binary_path.c_str()));

  auto retry = Value(fl_value_new_map());
  MapSetString(retry.get(), "domain", "files");
  MapSetString(retry.get(), "recordId", binary_path.string());
  g_autoptr(FlValue) retry_arguments = RequestArguments(retry.release());
  g_autoptr(FlValue) retry_response =
      InvokeHost(messenger, "openBinary", retry_arguments);
  FlValue* retry_payload = fl_value_get_list_value(retry_response, 0);
  g_assert_cmpstr(fl_value_get_string(FindValue(retry_payload, "handleId")), ==,
                  "linux_handle_1");

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}
