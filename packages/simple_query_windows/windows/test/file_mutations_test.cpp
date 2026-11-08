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

// Purpose: Name one mutation to run against a folder. Every field is
// borrowed, so it must not outlive the call that builds it.
// @param request is the request as Dart would send it.
// @param domain is "files" or "media".
// @param root is the folder a filtered mutation searches.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FolderMutation {
  const EncodableMap& request;
  const std::string& domain;
  const fs::path& root;
};

// Purpose: Run one mutation against a folder.
// @param mutation names the request, the domain and the folder.
// @returns The mutation's result.
// @throws Nothing.
ErrorOr<EncodableMap> Mutate(const FolderMutation& mutation) {
  const std::function<fs::path()> resolve_root = [root = mutation.root]() { return root; };
  return MutateFiles(
      {.request = mutation.request, .domain = mutation.domain, .resolve_root = resolve_root});
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

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
