// Native tests for files and media mutations: inserts, filtered deletes and
// updates for both domains, and the errors a malformed request or an
// unreadable folder returns.

#include "file_mutations.h"

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "boundary_formats.h"
#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
namespace fs = std::filesystem;

// Purpose: Describe one filter of a mutation request.
// @param field is sent as the filter's "field".
// @param op is the operator's wire name, such as "equals".
// @param value is sent as the filter's "value".
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FilterSpec {
  std::string field;
  std::string op;
  EncodableValue value;
};

// Purpose: Build a filters list holding one filter.
// @param filter is the filter.
// @returns The filters value.
// @throws Nothing.
EncodableValue OneFilter(const FilterSpec& filter) {
  return EncodableValue(EncodableList{EncodableValue(
      EncodableMap{{EncodableValue("field"), EncodableValue(filter.field)},
                   {EncodableValue("operator"), EncodableValue(filter.op)},
                   {EncodableValue("value"), filter.value}})});
}

// Purpose: Read a whole file as text.
// @param path is the file.
// @returns The file's contents, or empty text when it cannot be opened.
// @throws Nothing.
std::string ReadText(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Purpose: Name one mutation to run against a folder. The request, domain
// and root are borrowed, so it must not outlive the call that builds it.
// @param request is the request as Dart would send it.
// @param domain is "files" or "media".
// @param root is the folder a filtered mutation searches.
// @param stop_requested ends subsequent mutation steps.
// @param write_effects inject native write/flush failures.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FolderMutation {
  const EncodableMap& request;
  const std::string& domain;
  const fs::path& root;
  StopRequested stop_requested = {};
  FileWriteEffects write_effects = {};
};

// Purpose: Run one mutation against a folder.
// @param mutation names the request, the domain and the folder.
// @returns The mutation's result.
// @throws Nothing.
ErrorOr<EncodableMap> Mutate(const FolderMutation& mutation) {
  const std::function<fs::path()> resolve_root = [root = mutation.root]() { return root; };
  return MutateFiles(
      {.request = mutation.request, .domain = mutation.domain, .resolve_root = resolve_root,
       .stop_requested = mutation.stop_requested, .write_effects = mutation.write_effects});
}

// Purpose: Read a successful mutation's affected count.
// @param result is the mutation's result.
// @returns The affected count.
// @throws std::bad_variant_access when the result has no integer count.
int64_t AffectedCount(const ErrorOr<EncodableMap>& result) {
  return std::get<int64_t>(result.value().at(EncodableValue("affectedCount")));
}

// Purpose: A filtered media delete removes the media whose uriOrPath matches,
// and leaves other media and non-media files alone.
TEST(FileMutationsTest, DeletesTheMediaAFilterMatches) {
  TempDirectory directory;
  const fs::path photo = directory.WriteFile("photo.jpg");
  const fs::path other = directory.WriteFile("other.jpg");
  const fs::path notes = directory.WriteFile("photo_notes.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "uriOrPath", .op = "contains", .value = EncodableValue("photo")})}};

  const auto result = Mutate({.request = request, .domain = "media", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_FALSE(fs::exists(photo));
  EXPECT_TRUE(fs::exists(other));
  EXPECT_TRUE(fs::exists(notes));
}

// Purpose: A filtered media update replaces the contents of the media whose
// uriOrPath matches.
TEST(FileMutationsTest, UpdatesTheMediaAFilterMatches) {
  TempDirectory directory;
  const fs::path photo = directory.WriteFile("photo.jpg");
  const fs::path other = directory.WriteFile("other.jpg");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"),
       EncodableValue(EncodableMap{{EncodableValue("content"), EncodableValue("replaced")}})},
      {EncodableValue("filters"),
       OneFilter({.field = "uriOrPath", .op = "contains", .value = EncodableValue("photo")})}};

  const auto result = Mutate({.request = request, .domain = "media", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_EQ(ReadText(photo), "replaced");
  EXPECT_EQ(ReadText(other), "changed");
}

// Purpose: A filtered files delete removes the files whose name matches.
TEST(FileMutationsTest, DeletesTheFilesAFilterMatches) {
  TempDirectory directory;
  const fs::path notes = directory.WriteFile("notes.txt");
  const fs::path kept = directory.WriteFile("kept.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "name", .op = "equals", .value = EncodableValue("notes.txt")})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_FALSE(fs::exists(notes));
  EXPECT_TRUE(fs::exists(kept));
}

// Purpose: An insert creates missing parent folders, writes the content, and
// returns the path it was given as insertedId.
TEST(FileMutationsTest, InsertsAFileWithItsContentAndParents) {
  TempDirectory directory;
  const fs::path target = directory.path() / "nested" / "new.txt";
  const std::string target_text = PathToUtf8(target);
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("insert")},
      {EncodableValue("values"),
       EncodableValue(EncodableMap{{EncodableValue("path"), EncodableValue(target_text)},
                                   {EncodableValue("content"), EncodableValue("hello")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_EQ(std::get<std::string>(result.value().at(EncodableValue("insertedId"))),
            target_text);
  EXPECT_EQ(ReadText(target), "hello");
}

// Purpose: An update with newPath moves the file it names.
TEST(FileMutationsTest, MovesAFileToItsNewPath) {
  TempDirectory directory;
  const fs::path original = directory.WriteFile("before.txt");
  const fs::path moved = directory.path() / "after" / "after.txt";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"),
       EncodableValue(
           EncodableMap{{EncodableValue("path"), EncodableValue(PathToUtf8(original))},
                        {EncodableValue("newPath"), EncodableValue(PathToUtf8(moved))}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_FALSE(fs::exists(original));
  EXPECT_EQ(ReadText(moved), "changed");
}

// Purpose: An insert without a path is rejected as invalid-query.
TEST(FileMutationsTest, RejectsAnInsertWithoutAPath) {
  TempDirectory directory;
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("insert")},
      {EncodableValue("values"),
       EncodableValue(EncodableMap{{EncodableValue("content"), EncodableValue("hello")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
}

// Purpose: Bytes that are not a Uint8List are rejected, not written as text.
TEST(FileMutationsTest, RejectsBytesThatAreNotAByteList) {
  TempDirectory directory;
  const fs::path notes = directory.WriteFile("notes.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"),
       EncodableValue(
           EncodableMap{{EncodableValue("path"), EncodableValue(PathToUtf8(notes))},
                        {EncodableValue("bytes"), EncodableValue("not bytes")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
  EXPECT_EQ(ReadText(notes), "changed");
}

// Purpose: A mutation type Dart does not send is rejected.
TEST(FileMutationsTest, RejectsAnUnknownMutationType) {
  TempDirectory directory;
  const EncodableMap request{{EncodableValue("type"), EncodableValue("upsert")}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
}

// Purpose: A filter with an operator Dart does not define is rejected before
// anything is deleted.
TEST(FileMutationsTest, RejectsAnUnknownFilterOperator) {
  TempDirectory directory;
  const fs::path notes = directory.WriteFile("notes.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "name", .op = "startsWith", .value = EncodableValue("notes")})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
  EXPECT_TRUE(fs::exists(notes));
}

// Purpose: A NaN filter value is rejected before anything is deleted. NaN
// has no order, so it must never widen a delete to every file.
TEST(FileMutationsTest, RejectsANanFilterBeforeDeleting) {
  TempDirectory directory;
  const fs::path notes = directory.WriteFile("notes.txt");
  const EncodableValue nan(std::numeric_limits<double>::quiet_NaN());
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"), OneFilter({.field = "size", .op = "equals", .value = nan})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
  EXPECT_TRUE(fs::exists(notes));
}

// Purpose: A NaN filter value is rejected before anything is updated.
TEST(FileMutationsTest, RejectsANanFilterBeforeUpdating) {
  TempDirectory directory;
  const fs::path photo = directory.WriteFile("photo.jpg");
  const EncodableValue nan(std::numeric_limits<double>::quiet_NaN());
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"),
       EncodableValue(EncodableMap{{EncodableValue("content"), EncodableValue("replaced")}})},
      {EncodableValue("filters"),
       OneFilter({.field = "size", .op = "notEquals", .value = nan})}};

  const auto result = Mutate({.request = request, .domain = "media", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
  EXPECT_EQ(ReadText(photo), "changed");
}

// Purpose: Malformed filters fail before a media delete can change any file.
TEST(FileMutationsTest, RejectsMalformedFiltersBeforeDeletingMedia) {
  const std::vector<EncodableValue> malformed{
      EncodableValue(EncodableMap{}), EncodableValue("filters"), EncodableValue(true),
      EncodableValue(int32_t{1}), EncodableValue(int64_t{1}), EncodableValue(1.5)};

  for (const auto& filters : malformed) {
    SCOPED_TRACE(filters.index());
    TempDirectory directory;
    const fs::path photo = directory.WriteFile("photo.jpg");
    const fs::path other = directory.WriteFile("other.jpg");
    const fs::path notes = directory.WriteFile("notes.txt");
    const EncodableMap request{
        {EncodableValue("type"), EncodableValue("delete")},
        {EncodableValue("filters"), filters},
    };

    const auto result = Mutate({.request = request, .domain = "media", .root = directory.path()});

    EXPECT_TRUE(result.has_error());
    if (result.has_error()) {
      EXPECT_EQ(result.error().code(), "invalid-query");
    }
    EXPECT_EQ(ReadText(photo), "changed");
    EXPECT_EQ(ReadText(other), "changed");
    EXPECT_EQ(ReadText(notes), "changed");
  }
}

// Purpose: Malformed filters fail before a media update can change any file.
TEST(FileMutationsTest, RejectsMalformedFiltersBeforeUpdatingMedia) {
  const std::vector<EncodableValue> malformed{
      EncodableValue(EncodableMap{}), EncodableValue("filters"), EncodableValue(true),
      EncodableValue(int32_t{1}), EncodableValue(int64_t{1}), EncodableValue(1.5)};

  for (const auto& filters : malformed) {
    SCOPED_TRACE(filters.index());
    TempDirectory directory;
    const fs::path photo = directory.WriteFile("photo.jpg");
    const fs::path other = directory.WriteFile("other.jpg");
    const fs::path notes = directory.WriteFile("notes.txt");
    const EncodableMap request{
        {EncodableValue("type"), EncodableValue("update")},
        {EncodableValue("filters"), filters},
        {EncodableValue("values"),
         EncodableValue(EncodableMap{{EncodableValue("content"), EncodableValue("replaced")}})},
    };

    const auto result = Mutate({.request = request, .domain = "media", .root = directory.path()});

    EXPECT_TRUE(result.has_error());
    if (result.has_error()) {
      EXPECT_EQ(result.error().code(), "invalid-query");
    }
    EXPECT_EQ(ReadText(photo), "changed");
    EXPECT_EQ(ReadText(other), "changed");
    EXPECT_EQ(ReadText(notes), "changed");
  }
}

// Purpose: A filtered mutation whose folder cannot be walked fails as
// unavailable instead of changing nothing and reporting success.
TEST(FileMutationsTest, FailsWhenTheFolderCannotBeWalked) {
  TempDirectory directory;
  const fs::path not_a_folder = directory.WriteFile("not_a_folder.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "name", .op = "equals", .value = EncodableValue("anything")})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = not_a_folder});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
}

// Purpose: A folder that cannot be resolved fails as unavailable.
TEST(FileMutationsTest, FailsWhenTheFolderCannotBeResolved) {
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "name", .op = "equals", .value = EncodableValue("anything")})}};
  const std::string domain = "files";
  const std::function<fs::path()> resolve_root = []() -> fs::path {
    throw std::system_error(std::make_error_code(std::errc::permission_denied),
                            "current directory");
  };

  const auto result =
      MutateFiles({.request = request, .domain = domain, .resolve_root = resolve_root});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
}

// Purpose: Recursive deletes count every removed entry without counting overlaps twice.
TEST(FileMutationsTest, CountsRecursiveDeletesIncludingOverlappingSelectedDescendants) {
  TempDirectory directory;
  const fs::path selected = directory.path() / "selected";
  fs::create_directories(selected / "selected_child");
  std::ofstream(selected / "selected_child" / "file.txt") << "keep count";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("delete")},
      {EncodableValue("filters"),
       OneFilter({.field = "name", .op = "contains", .value = EncodableValue("selected")})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 3);
  EXPECT_FALSE(fs::exists(selected));
}

// Purpose: Invalid bytes cannot move a file before the request is rejected.
TEST(FileMutationsTest, ValidatesBytesBeforeMovingAnExplicitTarget) {
  TempDirectory directory;
  const auto original = directory.WriteFile("original.txt");
  const auto destination = directory.path() / "new_parent" / "moved.txt";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(original))},
          {EncodableValue("newPath"), EncodableValue(PathToUtf8(destination))},
          {EncodableValue("bytes"), EncodableValue("invalid bytes")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "invalid-query");
  EXPECT_EQ(ReadText(original), "changed");
  EXPECT_FALSE(fs::exists(destination.parent_path()));
}

// Purpose: Explicit path and id targets cannot bypass malformed or NaN filters.
TEST(FileMutationsTest, ValidatesFiltersBeforeExplicitPathOrIdUpdates) {
  TempDirectory directory;
  const auto target = directory.WriteFile("original.txt");
  const std::vector<EncodableValue> invalid_filters{
      EncodableValue("invalid filters"),
      OneFilter({.field = "size", .op = "equals",
                 .value = EncodableValue(std::numeric_limits<double>::quiet_NaN())})};
  for (const auto* key : {"path", "id"}) {
    for (const auto& filters : invalid_filters) {
      const EncodableMap request{
          {EncodableValue("type"), EncodableValue("update")},
          {EncodableValue("filters"), filters},
          {EncodableValue("values"), EncodableValue(EncodableMap{
              {EncodableValue(key), EncodableValue(PathToUtf8(target))},
              {EncodableValue("content"), EncodableValue("replaced")}})}};
      const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.error().code(), "invalid-query");
      EXPECT_EQ(ReadText(target), "changed");
    }
  }
}

// Purpose: Insert never overwrites an existing file, even without new contents.
TEST(FileMutationsTest, ExistingInsertPreservesOriginalWithAndWithoutContent) {
  TempDirectory directory;
  const auto target = directory.WriteFile("existing.txt");
  for (const bool provide_content : {false, true}) {
    EncodableMap values{{EncodableValue("path"), EncodableValue(PathToUtf8(target))}};
    if (provide_content) {
      values[EncodableValue("content")] = EncodableValue("replacement");
    }
    const EncodableMap request{{EncodableValue("type"), EncodableValue("insert")},
                               {EncodableValue("values"), EncodableValue(values)}};
    const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code(), "unavailable");
    EXPECT_EQ(ReadText(target), "changed");
    EXPECT_EQ(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator()), 1);
  }
}

// Purpose: A destination created after staging wins over an exclusive insert.
TEST(FileMutationsTest, ConcurrentInsertCannotOverwriteTheWinningFile) {
  TempDirectory directory;
  const auto target = directory.path() / "new.txt";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("insert")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("losing insert")}})}};
  FileWriteEffects effects;
  effects.flush = [&](void*) {
    std::ofstream(target) << "winning insert";
    return std::error_code{};
  };

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .write_effects = effects});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_EQ(ReadText(target), "winning insert");
  EXPECT_EQ(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator()), 1);
}

// Purpose: Partial write and flush failures cannot truncate the existing file.
TEST(FileMutationsTest, PreservesOriginalOnPartialWriteAndFlushFailures) {
  for (const bool fail_flush : {false, true}) {
    TempDirectory directory;
    const auto target = directory.WriteFile("original.txt");
    const EncodableMap request{
        {EncodableValue("type"), EncodableValue("update")},
        {EncodableValue("values"), EncodableValue(EncodableMap{
            {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
            {EncodableValue("content"), EncodableValue("replacement bytes")}})}};
    FileWriteEffects effects;
    if (fail_flush) {
      effects.flush = [](void*) { return std::make_error_code(std::errc::io_error); };
    } else {
      effects.write = [](void* handle, const std::string& bytes) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), 2, &written, nullptr)) {
          return std::error_code(static_cast<int>(GetLastError()), std::system_category());
        }
        EXPECT_EQ(written, 2u);
        return std::make_error_code(std::errc::no_space_on_device);
      };
    }
    const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                                .write_effects = effects});
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code(), "unavailable");
    EXPECT_NE(result.error().message().find(fail_flush ? "flush staging" : "write staging"),
              std::string::npos);
    EXPECT_EQ(ReadText(target), "changed");
    EXPECT_EQ(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator()), 1);
  }
}

// Purpose: Cancellation after one update stops later targets and reports completed work.
TEST(FileMutationsTest, StopsBetweenUpdateTargetsAndReportsConfirmedCount) {
  TempDirectory directory;
  const auto first = directory.WriteFile("a.txt");
  const auto second = directory.WriteFile("b.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("content"), EncodableValue("replaced")}})}};
  const auto stop = [&]() { return ReadText(first) == "replaced" || ReadText(second) == "replaced"; };

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .stop_requested = stop});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_NE(result.error().message().find("stopped"), std::string::npos);
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 1);
  EXPECT_NE(ReadText(first) == "replaced", ReadText(second) == "replaced");
  EXPECT_NE(ReadText(first) == "changed", ReadText(second) == "changed");
}

// Purpose: Cancellation after one delete stops later targets and reports completed work.
TEST(FileMutationsTest, StopsBetweenDeleteTargetsAndReportsConfirmedCount) {
  TempDirectory directory;
  const auto first = directory.WriteFile("a.txt");
  const auto second = directory.WriteFile("b.txt");
  const EncodableMap request{{EncodableValue("type"), EncodableValue("delete")}};
  const auto stop = [&]() { return !fs::exists(first) || !fs::exists(second); };

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .stop_requested = stop});

  ASSERT_TRUE(result.has_error());
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 1);
  EXPECT_NE(fs::exists(first), fs::exists(second));
}

// Purpose: Cancellation before publication preserves the original and removes staging data.
TEST(FileMutationsTest, StopsAfterFlushWithoutPublishingReplacement) {
  TempDirectory directory;
  const auto target = directory.WriteFile("original.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("replacement")}})}};
  bool stopped = false;
  FileWriteEffects effects;
  effects.flush = [&](void*) { stopped = true; return std::error_code{}; };
  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .stop_requested = [&]() { return stopped; },
                              .write_effects = effects});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(ReadText(target), "changed");
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 0);
  EXPECT_EQ(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator()), 1);
}

// Purpose: A completed move is disclosed when the following replacement fails.
TEST(FileMutationsTest, ReportsMoveCompletedBeforeWriteFailure) {
  TempDirectory directory;
  const auto original = directory.WriteFile("original.txt");
  const auto moved = directory.path() / "moved.txt";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(original))},
          {EncodableValue("newPath"), EncodableValue(PathToUtf8(moved))},
          {EncodableValue("content"), EncodableValue("replacement")}})}};
  FileWriteEffects effects;
  effects.write = [](void*, const std::string&) {
    return std::make_error_code(std::errc::no_space_on_device);
  };

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .write_effects = effects});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_FALSE(fs::exists(original));
  EXPECT_EQ(ReadText(moved), "changed");
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 1);
  EXPECT_TRUE(std::get<bool>(details.at(EncodableValue("partialEffectsPossible"))));
}

// Purpose: Cancellation during target discovery prevents every mutation.
TEST(FileMutationsTest, StopsDuringTargetDiscovery) {
  TempDirectory directory;
  const auto target = directory.WriteFile("original.txt");
  const EncodableMap request{{EncodableValue("type"), EncodableValue("delete")}};
  const std::string domain = "files";
  bool stopped = false;
  const std::function<fs::path()> root = [&]() {
    stopped = true;
    return directory.path();
  };
  const auto result = MutateFiles({.request = request, .domain = domain, .resolve_root = root,
                                   .stop_requested = [&]() { return stopped; }});

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_NE(result.error().message().find("stopped"), std::string::npos);
  EXPECT_EQ(ReadText(target), "changed");
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 0);
}

// Purpose: Content updates follow a symbolic link while retaining the link itself.
TEST(FileMutationsTest, UpdatesSymbolicLinkTargetWithoutReplacingTheLink) {
  TempDirectory directory;
  const auto target = directory.WriteFile("target.txt");
  const auto link = directory.path() / "link.txt";
  std::error_code error;
  fs::create_symlink(target, link, error);
  if (error == std::errc::operation_not_permitted || error == std::errc::permission_denied) {
    GTEST_SKIP() << "OS symbolic-link permission unavailable: " << error.message();
  }
  ASSERT_FALSE(error) << error.message();
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(link))},
          {EncodableValue("content"), EncodableValue("updated target")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
  EXPECT_EQ(ReadText(target), "updated target");
  EXPECT_EQ(ReadText(link), "updated target");
}

// Purpose: Exclusive inserts reject both existing and dangling symbolic links.
TEST(FileMutationsTest, InsertRejectsExistingAndDanglingSymbolicLinks) {
  TempDirectory directory;
  const auto target = directory.WriteFile("target.txt");
  for (const bool dangling : {false, true}) {
    const auto link = directory.path() / (dangling ? "dangling.txt" : "link.txt");
    const auto destination = dangling ? directory.path() / "missing.txt" : target;
    std::error_code error;
    fs::create_symlink(destination, link, error);
    if (error == std::errc::operation_not_permitted || error == std::errc::permission_denied) {
      GTEST_SKIP() << "OS symbolic-link permission unavailable: " << error.message();
    }
    ASSERT_FALSE(error) << error.message();
    const EncodableMap request{
        {EncodableValue("type"), EncodableValue("insert")},
        {EncodableValue("values"), EncodableValue(EncodableMap{
            {EncodableValue("path"), EncodableValue(PathToUtf8(link))},
            {EncodableValue("content"), EncodableValue("must not replace link")}})}};
    const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code(), "unavailable");
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    EXPECT_EQ(ReadText(target), "changed");
    if (dangling) {
      EXPECT_FALSE(fs::exists(destination));
    }
  }
}

// Purpose: Missing explicit update targets remain successful zero-effect updates.
TEST(FileMutationsTest, MissingUpdateTargetDoesNotCreateAFile) {
  TempDirectory directory;
  const auto target = directory.path() / "missing.txt";
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("must not create")}})}};
  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});
  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 0);
  EXPECT_FALSE(fs::exists(target));
  EXPECT_TRUE(fs::is_empty(directory.path()));
}

// Purpose: Replacement failures retain an original at its name or a disclosed recovery path.
TEST(FileMutationsTest, ReplaceFailureRetainsAndDisclosesTheOriginal) {
  for (const bool original_already_moved : {false, true}) {
    TempDirectory directory;
    const auto target = directory.WriteFile("original.txt");
    const EncodableMap request{
        {EncodableValue("type"), EncodableValue("update")},
        {EncodableValue("values"), EncodableValue(EncodableMap{
            {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
            {EncodableValue("content"), EncodableValue("replacement")}})}};
    FileWriteEffects effects;
    effects.replace = [original_already_moved](const FileReplacement& replacement) {
      if (original_already_moved) {
        std::error_code error;
        fs::rename(replacement.original, replacement.backup, error);
        if (error) {
          return error;
        }
      }
      return std::make_error_code(std::errc::io_error);
    };
    const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                                .write_effects = effects});
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code(), "unavailable");
    const auto& details = std::get<EncodableMap>(result.error().details());
    const auto recovery = PathFromUtf8(std::get<std::string>(
        details.at(EncodableValue("recoveryPath"))));
    EXPECT_TRUE(fs::is_directory(recovery));
    EXPECT_NE(result.error().message().find(PathToUtf8(recovery)), std::string::npos);
    EXPECT_EQ(ReadText(original_already_moved ? recovery / "original" : target), "changed");
  }
}

// Purpose: Windows replacements preserve the original creation time and named streams.
TEST(FileMutationsTest, ReplacementPreservesCreationTimeAndNamedStreams) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Requires actual Windows file metadata and named streams";
#else
  TempDirectory directory;
  const auto target = directory.WriteFile("original.txt");
  fs::path stream = target;
  stream += ":unify-preserved";
  std::ofstream(stream) << "original stream";
  const HANDLE before_handle = CreateFileW(target.c_str(),
      FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(before_handle, INVALID_HANDLE_VALUE);
  const FILETIME creation{0, 0x01d00000};
  const bool set_time = SetFileTime(before_handle, &creation, nullptr, nullptr) != 0;
  EXPECT_TRUE(CloseHandle(before_handle));
  ASSERT_TRUE(set_time);
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("replacement")}})}};
  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});
  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(ReadText(target), "replacement");
  EXPECT_EQ(ReadText(stream), "original stream");
  const HANDLE after_handle = CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(after_handle, INVALID_HANDLE_VALUE);
  FILETIME actual{};
  const bool read_time = GetFileTime(after_handle, &actual, nullptr, nullptr) != 0;
  EXPECT_TRUE(CloseHandle(after_handle));
  ASSERT_TRUE(read_time);
  EXPECT_EQ(actual.dwLowDateTime, creation.dwLowDateTime);
  EXPECT_EQ(actual.dwHighDateTime, creation.dwHighDateTime);
#endif
}

// Purpose: Backup cleanup failure reports the published update and retains original data.
TEST(FileMutationsTest, BackupCleanupFailureDisclosesPublishedUpdateAndRecovery) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Requires actual Windows sharing restrictions";
#else
  TempDirectory directory;
  const auto target = directory.WriteFile("original.txt");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("replacement")}})}};
  FileWriteEffects effects;
  HANDLE held_backup = INVALID_HANDLE_VALUE;
  fs::path backup;
  effects.replace = [&](const FileReplacement& replacement) {
    if (!ReplaceFileW(replacement.original.c_str(), replacement.replacement.c_str(),
                      replacement.backup.c_str(), 0, nullptr, nullptr)) {
      return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
    backup = replacement.backup;
    held_backup = CreateFileW(backup.c_str(), GENERIC_READ, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(held_backup, INVALID_HANDLE_VALUE);
    return std::error_code{};
  };
  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path(),
                              .write_effects = effects});
  if (held_backup != INVALID_HANDLE_VALUE) {
    EXPECT_TRUE(CloseHandle(held_backup));
  }
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_EQ(ReadText(target), "replacement");
  EXPECT_EQ(ReadText(backup), "changed");
  const auto& details = std::get<EncodableMap>(result.error().details());
  EXPECT_EQ(std::get<int64_t>(details.at(EncodableValue("affectedCount"))), 1);
  EXPECT_EQ(std::get<std::string>(details.at(EncodableValue("recoveryPath"))),
            PathToUtf8(backup.parent_path()));
#endif
}

// Purpose: Name a valid maximum-length component, allowing its full Windows path.
// @param parent is the temporary directory that will contain the file.
// @returns A 255-character filename; Windows uses its extended-length path form.
// @throws std::filesystem::filesystem_error when the absolute parent cannot be read.
fs::path LongComponentPath(const fs::path& parent) {
  fs::path target = fs::absolute(parent / (std::string(251, 'x') + ".txt"));
#if defined(_WIN32)
  const std::wstring native = target.native();
  const bool already_extended = native.rfind(L"\\\\?\\", 0) == 0;
  if (!already_extended) {
    const bool unc = native.rfind(L"\\\\", 0) == 0;
    target = fs::path(unc ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native);
  }
#endif
  return target;
}

// Purpose: An insert accepts a valid 255-character destination component.
TEST(FileMutationsTest, InsertsFileWithMaximumLengthComponent) {
  TempDirectory directory;
  const fs::path target = LongComponentPath(directory.path());
  ASSERT_EQ(target.filename().native().size(), 255u);
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("insert")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("inserted long name")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_EQ(ReadText(target), "inserted long name");
  EXPECT_TRUE(fs::remove(target));
  EXPECT_TRUE(fs::is_empty(directory.path()));
}

// Purpose: An update accepts the same valid component without lengthening staging names.
TEST(FileMutationsTest, UpdatesFileWithMaximumLengthComponent) {
  TempDirectory directory;
  const fs::path target = LongComponentPath(directory.path());
  ASSERT_EQ(target.filename().native().size(), 255u);
  std::ofstream(target) << "original long name";
  ASSERT_EQ(ReadText(target), "original long name");
  const EncodableMap request{
      {EncodableValue("type"), EncodableValue("update")},
      {EncodableValue("values"), EncodableValue(EncodableMap{
          {EncodableValue("path"), EncodableValue(PathToUtf8(target))},
          {EncodableValue("content"), EncodableValue("updated long name")}})}};

  const auto result = Mutate({.request = request, .domain = "files", .root = directory.path()});

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(AffectedCount(result), 1);
  EXPECT_EQ(ReadText(target), "updated long name");
  EXPECT_TRUE(fs::remove(target));
  EXPECT_TRUE(fs::is_empty(directory.path()));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
