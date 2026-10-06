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
