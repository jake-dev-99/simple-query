#pragma once

/** Purpose: Exercise scoped mutations through real serialized native callbacks.
 * @param domain selects files or media. @param type selects the mutation.
 * @returns An owned request with replacement content and empty filters.
 * @throws std::bad_alloc when fixture strings are allocated. */
ValuePtr ScopedMutationRequest(const char* domain, const char* type) {
  auto request = Value(fl_value_new_map());
  MapSetString(request.get(), "domain", domain);
  MapSetString(request.get(), "type", type);
  MapSet(request.get(), "filters", fl_value_new_list());
  auto values = Value(fl_value_new_map());
  MapSetString(values.get(), "content", "after");
  MapSet(request.get(), "values", values.release());
  return request;
}

/** Purpose: Bound a fixture independently of its target and rename inputs.
 * @param request receives platform data. @param root is the allowed directory.
 * @returns Nothing. @throws Nothing. */
void SetMutationRoot(FlValue* request, const std::filesystem::path& root) {
  auto platform = Value(fl_value_new_map());
  MapSetString(platform.get(), "rootPath", root.string());
  MapSet(request, "platformData", platform.release());
}

/** Purpose: Verify rejected mutations cannot silently change real file bytes.
 * @param path identifies the sentinel. @param expected is its literal content.
 * @returns Nothing. @throws Nothing. */
void AssertFileContent(const std::filesystem::path& path, const char* expected) {
  std::ifstream input(path);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  g_assert_cmpstr(contents.c_str(), ==, expected);
}

/** Purpose: Require structured rejection through the production callback.
 * @param messenger is the registered transport. @param request transfers input.
 * @returns Nothing. @throws Nothing. */
void AssertScopeRejected(TestBinaryMessenger* messenger, ValuePtr request) {
  g_autoptr(FlValue) arguments = RequestArguments(request.release());
  g_autoptr(FlValue) response = InvokeHost(messenger, "mutate", arguments);
  const std::string code = ResponseErrorCode(response);
  g_assert_cmpstr(code.c_str(), ==, "invalid-query");
}

/** Purpose: Prevent every unrooted file/media write from using working scope.
 * @param None. @returns Nothing. @throws Nothing. */
void TestAllMutationsRequireRoot() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path root(temporary);
  const auto sentinel = root / "sentinel.jpg";
  std::ofstream(sentinel) << "before";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  for (const char* domain : {"files", "media"}) {
    for (const char* type : {"update", "insert", "delete"}) {
      auto request = ScopedMutationRequest(domain, type);
      MapSetString(FindValue(request.get(), "values"), "path", sentinel.string());
      AssertScopeRejected(messenger, std::move(request));
      AssertFileContent(sentinel, "before");
    }
  }
  ClearHost(messenger);
  std::filesystem::remove_all(root);
}

/** Purpose: Reject absolute target paths and IDs outside the requested root.
 * @param None. @returns Nothing. @throws Nothing. */
void TestMutationRejectsOutsideTargets() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path fixture(temporary);
  const auto root = fixture / "root";
  std::filesystem::create_directory(root);
  const auto outside = fixture / "root-sibling.jpg";
  std::ofstream(outside) << "before";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  for (const char* domain : {"files", "media"}) {
    for (const char* key : {"path", "id"}) {
      auto update = ScopedMutationRequest(domain, "update");
      SetMutationRoot(update.get(), root);
      MapSetString(FindValue(update.get(), "values"), key, outside.string());
      AssertScopeRejected(messenger, std::move(update));
      AssertFileContent(outside, "before");
    }
    auto insert = ScopedMutationRequest(domain, "insert");
    SetMutationRoot(insert.get(), root);
    MapSetString(FindValue(insert.get(), "values"), "path", "../new-outside.jpg");
    AssertScopeRejected(messenger, std::move(insert));
    g_assert_false(std::filesystem::exists(fixture / "new-outside.jpg"));
  }
  ClearHost(messenger);
  std::filesystem::remove_all(fixture);
}

/** Purpose: Reject rename destinations before changing either source or target.
 * @param None. @returns Nothing. @throws Nothing. */
void TestMutationRejectsOutsideRenames() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path fixture(temporary);
  const auto root = fixture / "root";
  std::filesystem::create_directory(root);
  const auto source = root / "source.jpg";
  std::ofstream(source) << "before";
  const auto absolute = fixture / "outside" / "destination.jpg";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  for (const char* domain : {"files", "media"}) {
    for (const auto& destination : {absolute.string(), std::string("../outside/destination.jpg")}) {
      auto update = ScopedMutationRequest(domain, "update");
      SetMutationRoot(update.get(), root);
      MapSetString(FindValue(update.get(), "values"), "path", "source.jpg");
      MapSetString(FindValue(update.get(), "values"), "newPath", destination);
      AssertScopeRejected(messenger, std::move(update));
      AssertFileContent(source, "before");
      g_assert_false(std::filesystem::exists(fixture / "outside"));
    }
  }
  ClearHost(messenger);
  std::filesystem::remove_all(fixture);
}

/** Purpose: Prevent existing symlink parents or targets from bypassing scope.
 * @param None. @returns Nothing. @throws Nothing. */
void TestMutationRejectsSymlinkEscapes() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path fixture(temporary);
  const auto root = fixture / "root";
  const auto outside = fixture / "outside";
  std::filesystem::create_directory(root);
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "sentinel.jpg") << "before";
  std::ofstream(root / "source.jpg") << "before";
  std::ofstream(root / "sentinel.jpg") << "before";
  std::ofstream(fixture / "sentinel.jpg") << "before";
  std::filesystem::create_directory(root / "nested");
  std::filesystem::create_symlink("../sentinel.jpg", root / "nested/link.jpg");
  std::filesystem::create_directory_symlink(outside, root / "escape");
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  for (const char* domain : {"files", "media"}) {
    for (const char* type : {"insert", "update"}) {
      auto request = ScopedMutationRequest(domain, type);
      SetMutationRoot(request.get(), root);
      MapSetString(FindValue(request.get(), "values"), "path", "escape/sentinel.jpg");
      AssertScopeRejected(messenger, std::move(request));
      AssertFileContent(outside / "sentinel.jpg", "before");
    }
    auto rename = ScopedMutationRequest(domain, "update");
    SetMutationRoot(rename.get(), root);
    MapSetString(FindValue(rename.get(), "values"), "path", "source.jpg");
    MapSetString(FindValue(rename.get(), "values"), "newPath", "escape/new.jpg");
    AssertScopeRejected(messenger, std::move(rename));
    AssertFileContent(root / "source.jpg", "before");
    g_assert_false(std::filesystem::exists(outside / "new.jpg"));
    auto moved_link = ScopedMutationRequest(domain, "update");
    SetMutationRoot(moved_link.get(), root);
    MapSetString(FindValue(moved_link.get(), "values"), "path", "nested/link.jpg");
    MapSetString(FindValue(moved_link.get(), "values"), "newPath", "moved-link.jpg");
    AssertScopeRejected(messenger, std::move(moved_link));
    g_assert_true(std::filesystem::is_symlink(root / "nested/link.jpg"));
    g_assert_false(std::filesystem::exists(root / "moved-link.jpg"));
    AssertFileContent(root / "sentinel.jpg", "before");
    AssertFileContent(fixture / "sentinel.jpg", "before");
  }
  ClearHost(messenger);
  std::filesystem::remove_all(fixture);
}

/** Purpose: Resolve valid relative paths and rename destinations under the root.
 * @param None. @returns Nothing. @throws Nothing. */
void TestMutationResolvesRelativePaths() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path root(temporary);
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  for (const char* domain : {"files", "media"}) {
    const std::string relative = std::string(domain) + "/source.jpg";
    auto insert = ScopedMutationRequest(domain, "insert");
    SetMutationRoot(insert.get(), root);
    MapSetString(FindValue(insert.get(), "values"), "path", relative);
    g_autoptr(FlValue) insert_args = RequestArguments(insert.release());
    g_autoptr(FlValue) inserted = InvokeHost(messenger, "mutate", insert_args);
    g_assert_cmpuint(fl_value_get_length(inserted), ==, 1);
    const auto absolute = root / relative;
    const std::string expected_id = absolute.string();
    g_assert_cmpstr(fl_value_get_string(FindValue(fl_value_get_list_value(inserted, 0), "insertedId")), ==, expected_id.c_str());
    AssertFileContent(absolute, "after");
    auto update = ScopedMutationRequest(domain, "update");
    SetMutationRoot(update.get(), root);
    MapSetString(FindValue(update.get(), "values"), "id", absolute.string());
    const std::string renamed = std::string(domain) + "/nested/renamed.jpg";
    MapSetString(FindValue(update.get(), "values"), "newPath", renamed);
    g_autoptr(FlValue) update_args = RequestArguments(update.release());
    g_autoptr(FlValue) updated = InvokeHost(messenger, "mutate", update_args);
    g_assert_cmpuint(fl_value_get_length(updated), ==, 1);
    g_assert_false(std::filesystem::exists(absolute));
    AssertFileContent(root / renamed, "after");
  }
  const auto target = root / "media/nested/renamed.jpg";
  const auto link = root / "delete-link.jpg";
  std::filesystem::create_symlink(target, link);
  auto removal = MediaMutation("delete", link, root);
  g_autoptr(FlValue) removal_args = RequestArguments(removal.release());
  g_autoptr(FlValue) removed = InvokeHost(messenger, "mutate", removal_args);
  g_assert_cmpuint(fl_value_get_length(removed), ==, 1);
  g_assert_false(std::filesystem::exists(link));
  AssertFileContent(target, "after");
  ClearHost(messenger);
  std::filesystem::remove_all(root);
}

/** Purpose: Preserve failed scoped writes while later valid batch writes run.
 * @param None. @returns Nothing. @throws Nothing. */
void TestBatchPreservesMutationScope() {
  g_autofree gchar* temporary = g_dir_make_tmp("simple-query-scope-XXXXXX", nullptr);
  const std::filesystem::path fixture(temporary);
  const auto root = fixture / "root";
  std::filesystem::create_directory(root);
  std::ofstream(fixture / "sentinel.jpg") << "before";
  g_autoptr(TestBinaryMessenger) messenger = NewMessenger();
  InstallHost(messenger);
  auto batch = Value(fl_value_new_map());
  SetMutationRoot(batch.get(), root);
  auto operations = Value(fl_value_new_list());
  for (const char* path : {"../sentinel.jpg", "nested/valid.jpg"}) {
    auto operation = ScopedMutationRequest("media", "insert");
    MapSetString(FindValue(operation.get(), "values"), "path", path);
    if (std::string(path) == "nested/valid.jpg") {
      // Per-operation options must not discard the batch's inherited root.
      auto platform = Value(fl_value_new_map());
      MapSetBool(platform.get(), "recursive", true);
      MapSet(operation.get(), "platformData", platform.release());
    }
    fl_value_append_take(operations.get(), operation.release());
  }
  MapSet(batch.get(), "operations", operations.release());
  g_autoptr(FlValue) arguments = RequestArguments(batch.release());
  g_autoptr(FlValue) response = InvokeHost(messenger, "batch", arguments);
  FlValue* results = FindValue(fl_value_get_list_value(response, 0), "results");
  g_assert_cmpuint(fl_value_get_length(results), ==, 2);
  FlValue* first = fl_value_get_list_value(results, 0);
  FlValue* error = FindValue(FindValue(first, "metadata"), "error");
  g_assert_nonnull(error);
  g_assert_cmpstr(fl_value_get_string(FindValue(error, "code")), ==, "invalidQuery");
  g_assert_cmpint(fl_value_get_int(FindValue(fl_value_get_list_value(results, 1), "affectedCount")), ==, 1);
  AssertFileContent(fixture / "sentinel.jpg", "before");
  AssertFileContent(root / "nested/valid.jpg", "after");
  ClearHost(messenger);
  std::filesystem::remove_all(fixture);
}
