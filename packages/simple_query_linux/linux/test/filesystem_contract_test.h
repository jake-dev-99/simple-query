#pragma once

/** Purpose: Invoke openBinary for one filesystem path through the real HostApi.
 * @param messenger is the registered fake transport.
 * @param path identifies the requested resource.
 * @returns A newly owned decoded Pigeon response.
 * @throws Nothing. */
FlValue* OpenBinaryPath(TestBinaryMessenger* messenger,
                        const std::filesystem::path& path) {
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  MapSetString(request.get(), "recordId", path.string());
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  return InvokeHost(messenger, "openBinary", arguments);
}

/** Purpose: Reject directories and stat-able unreadable files before a handle
 * is published.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestOpenBinaryRequiresReadableRegularFile() {
  if (geteuid() == 0) {
    g_test_skip("Permission isolation requires a non-root test process.");
    return;
  }
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-binary-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  const std::filesystem::path directory = root / "directory";
  const std::filesystem::path fifo = root / "stream.fifo";
  const std::filesystem::path file = root / "payload.bin";
  std::filesystem::create_directory(directory);
  g_assert_cmpint(mkfifo(fifo.c_str(), 0600), ==, 0);
  std::ofstream(file, std::ios::binary) << "payload";

  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  g_autoptr(FlValue) directory_response =
      OpenBinaryPath(messenger, directory);
  const std::string directory_error = ResponseErrorCode(directory_response);
  g_assert_cmpstr(directory_error.c_str(), ==, "unavailable");

  const gint64 fifo_started = g_get_monotonic_time();
  g_autoptr(FlValue) fifo_response = OpenBinaryPath(messenger, fifo);
  const gint64 fifo_elapsed = g_get_monotonic_time() - fifo_started;
  const std::string fifo_error = ResponseErrorCode(fifo_response);
  g_assert_cmpstr(fifo_error.c_str(), ==, "unavailable");
  g_assert_cmpint(fifo_elapsed, <, G_USEC_PER_SEC);

  g_assert_cmpint(g_chmod(file.c_str(), 0), ==, 0);
  g_autoptr(FlValue) unreadable_response = OpenBinaryPath(messenger, file);
  const std::string unreadable_error = ResponseErrorCode(unreadable_response);
  const std::string unreadable_message =
      ResponseErrorMessage(unreadable_response);
  g_assert_cmpstr(unreadable_error.c_str(), ==, "unavailable");
  g_assert_nonnull(
      g_strstr_len(unreadable_message.c_str(), -1, file.c_str()));

  g_assert_cmpint(g_chmod(file.c_str(), 0600), ==, 0);
  g_autoptr(FlValue) retry_response = OpenBinaryPath(messenger, file);
  FlValue* payload = fl_value_get_list_value(retry_response, 0);
  g_assert_cmpstr(fl_value_get_string(FindValue(payload, "handleId")), ==,
                  "linux_handle_1");

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

/** Purpose: Build a file query with one requested size sort.
 * @param root bounds the query.
 * @param direction is ascending or descending.
 * @returns A newly owned query request.
 * @throws Nothing. */
ValuePtr SizeSortedQuery(const std::filesystem::path& root,
                         const char* direction) {
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "files");
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  auto sort = Value(fl_value_new_list());
  auto descriptor = Value(fl_value_new_map());
  MapSetString(descriptor.get(), "field", "size");
  MapSetString(descriptor.get(), "direction", direction);
  fl_value_append_take(sort.get(), descriptor.release());
  MapSet(request.get(), "sort", sort.release());
  return request;
}

/** Purpose: Assert the real query channel sorts integer sizes numerically.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestNumericSortUsesNumericOrder() {
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-sort-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  std::ofstream(root / "two.bin") << "12";
  std::ofstream(root / "nine.bin") << "123456789";
  std::ofstream(root / "ten.bin") << "1234567890";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  const std::array<int64_t, 3> ascending = {2, 9, 10};
  const std::array<int64_t, 3> descending = {10, 9, 2};
  for (const auto& [direction, expected] :
       {std::pair<const char*, const std::array<int64_t, 3>*>("ascending",
                                                              &ascending),
        std::pair<const char*, const std::array<int64_t, 3>*>("descending",
                                                              &descending)}) {
    auto request = SizeSortedQuery(root, direction);
    g_autoptr(FlValue) arguments = RequestArguments(request.release());
    g_autoptr(FlValue) response = InvokeHost(messenger, "query", arguments);
    FlValue* rows =
        FindValue(fl_value_get_list_value(response, 0), "records");
    g_assert_cmpuint(fl_value_get_length(rows), ==, expected->size());
    for (size_t index = 0; index < expected->size(); index++) {
      FlValue* row = fl_value_get_list_value(rows, index);
      g_assert_cmpint(fl_value_get_int(FindValue(row, "size")), ==,
                      (*expected)[index]);
    }
  }

  Rows mixed_rows;
  for (FlValue* value :
       {fl_value_new_int(9007199254740993LL),
        fl_value_new_int(9007199254740992LL), fl_value_new_float(2.5),
        fl_value_new_string("1")}) {
    auto row = Value(fl_value_new_map());
    MapSet(row.get(), "rank", value);
    mixed_rows.push_back(std::move(row));
  }
  auto mixed_sort = Value(fl_value_new_list());
  auto mixed_descriptor = Value(fl_value_new_map());
  MapSetString(mixed_descriptor.get(), "field", "rank");
  MapSetString(mixed_descriptor.get(), "direction", "ascending");
  fl_value_append_take(mixed_sort.get(), mixed_descriptor.release());
  ApplySort(&mixed_rows, mixed_sort.get());
  g_assert_cmpfloat(fl_value_get_float(FindValue(mixed_rows[0].get(), "rank")),
                    ==, 2.5);
  g_assert_cmpint(fl_value_get_int(FindValue(mixed_rows[1].get(), "rank")), ==,
                  9007199254740992LL);
  g_assert_cmpint(fl_value_get_int(FindValue(mixed_rows[2].get(), "rank")), ==,
                  9007199254740993LL);
  g_assert_cmpstr(fl_value_get_string(FindValue(mixed_rows[3].get(), "rank")),
                  ==, "1");

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

/** Purpose: Invoke one unfiltered delete without a filesystem root.
 * @param messenger is the registered fake transport.
 * @param domain selects files or media.
 * @returns A newly owned decoded response.
 * @throws Nothing. */
FlValue* DeleteWithoutRoot(TestBinaryMessenger* messenger,
                           const char* domain) {
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", domain);
  MapSetString(request.get(), "type", "delete");
  MapSet(request.get(), "filters", fl_value_new_list());
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  return InvokeHost(messenger, "mutate", arguments);
}

/** Purpose: Prevent unrooted deletes from targeting process working data.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestDeleteRequiresRootForFilesAndMedia() {
  g_autofree gchar* original_directory = g_get_current_dir();
  for (const char* domain : {"files", "media"}) {
    g_autofree gchar* temporary =
        g_dir_make_tmp("simple-query-delete-root-XXXXXX", nullptr);
    g_assert_nonnull(temporary);
    const std::filesystem::path root(temporary);
    const std::filesystem::path sentinel =
        root / (std::string(domain) + (std::string(domain) == "media"
                                           ? ".jpg"
                                           : ".txt"));
    std::ofstream(sentinel) << "preserve";
    g_assert_cmpint(g_chdir(root.c_str()), ==, 0);

    g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
    InstallHost(messenger);
    g_autoptr(FlValue) response = DeleteWithoutRoot(messenger, domain);
    const std::string error_code = ResponseErrorCode(response);
    g_assert_cmpstr(error_code.c_str(), ==, "invalid-query");
    g_assert_true(std::filesystem::exists(sentinel));
    ClearHost(messenger);
    g_assert_cmpint(g_chdir(original_directory), ==, 0);
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
  }
}

/** Purpose: Build one filtered media mutation using the portable record ID.
 * @param type selects update or delete.
 * @param path is the target media path.
 * @param root bounds target discovery.
 * @param content optionally supplies replacement content.
 * @returns A newly owned mutation request.
 * @throws Nothing. */
ValuePtr MediaMutation(const char* type, const std::filesystem::path& path,
                       const std::filesystem::path& root,
                       const char* content = nullptr) {
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", "media");
  MapSetString(request.get(), "type", type);
  auto filter = Value(fl_value_new_map());
  MapSetString(filter.get(), "field", "id");
  MapSetString(filter.get(), "operator", "equals");
  MapSetString(filter.get(), "value", path.string());
  auto filters = Value(fl_value_new_list());
  fl_value_append_take(filters.get(), filter.release());
  MapSet(request.get(), "filters", filters.release());
  auto platform_data = Value(fl_value_new_map());
  MapSetString(platform_data.get(), "rootPath", root.string());
  MapSet(request.get(), "platformData", platform_data.release());
  if (content != nullptr) {
    auto values = Value(fl_value_new_map());
    MapSetString(values.get(), "content", content);
    MapSet(request.get(), "values", values.release());
  }
  return request;
}

/** Purpose: Make filtered media update and delete affect their real files.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestMediaMutationUsesPortableRecordPath() {
  g_autofree gchar* temporary =
      g_dir_make_tmp("simple-query-media-mutation-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const std::filesystem::path root(temporary);
  const std::filesystem::path update_path = root / "update.jpg";
  const std::filesystem::path delete_path = root / "delete.jpg";
  std::ofstream(update_path) << "before";
  std::ofstream(delete_path) << "remove";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);

  auto update = MediaMutation("update", update_path, root, "after");
  g_autoptr(FlValue) update_args = RequestArguments(update.release());
  g_autoptr(FlValue) update_response =
      InvokeHost(messenger, "mutate", update_args);
  FlValue* update_result = fl_value_get_list_value(update_response, 0);
  g_assert_cmpint(fl_value_get_int(FindValue(update_result, "affectedCount")),
                  ==, 1);
  std::ifstream updated(update_path);
  std::string updated_content;
  updated >> updated_content;
  g_assert_cmpstr(updated_content.c_str(), ==, "after");

  auto removal = MediaMutation("delete", delete_path, root);
  g_autoptr(FlValue) removal_args = RequestArguments(removal.release());
  g_autoptr(FlValue) removal_response =
      InvokeHost(messenger, "mutate", removal_args);
  FlValue* removal_result = fl_value_get_list_value(removal_response, 0);
  g_assert_cmpint(fl_value_get_int(FindValue(removal_result, "affectedCount")),
                  ==, 1);
  g_assert_false(std::filesystem::exists(delete_path));

  ClearHost(messenger);
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}
