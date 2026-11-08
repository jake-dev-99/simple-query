// Native tests for record listing and fingerprints: which ids changed
// between snapshots, how a row's fingerprint follows its fields, the file
// walk, and the path and time formats rows carry to Dart.

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <any>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boundary_formats.h"
#include "file_records.h"
#include "polling_observers.h"
#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using namespace std::chrono_literals;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Changed ids cover changed, removed and added records.
TEST(ChangedIdsTest, ListsChangedRemovedAndAddedIds) {
  const Snapshot before{{"changed", 1}, {"removed", 1}, {"same", 1}};
  const Snapshot after{{"added", 1}, {"changed", 2}, {"same", 1}};

  std::vector<std::string> ids;
  for (const auto& id : ChangedIds(before, after)) {
    ids.push_back(std::get<std::string>(id));
  }

  EXPECT_EQ(ids, (std::vector<std::string>{"added", "changed", "removed"}));
}

// Purpose: Equal rows give equal fingerprints.
TEST(FingerprintRecordsTest, GivesEqualRowsEqualFingerprints) {
  const EncodableMap row{{EncodableValue("id"), EncodableValue("a")},
                         {EncodableValue("title"), EncodableValue("Lunch")}};

  EXPECT_EQ(FingerprintRecords({row}), FingerprintRecords({row}));
}

// Purpose: Changing any field changes the row's fingerprint.
TEST(FingerprintRecordsTest, ChangesWhenAnyFieldChanges) {
  const EncodableMap before{{EncodableValue("id"), EncodableValue("a")},
                            {EncodableValue("title"), EncodableValue("Lunch")}};
  EncodableMap after = before;
  after[EncodableValue("title")] = EncodableValue("Dinner");

  EXPECT_NE(FingerprintRecords({before}).at("a"), FingerprintRecords({after}).at("a"));
}

// Purpose: A row without a string id is left out.
TEST(FingerprintRecordsTest, LeavesOutRowsWithoutAStringId) {
  const EncodableMap no_id{{EncodableValue("title"), EncodableValue("Lunch")}};
  const EncodableMap number_id{{EncodableValue("id"), EncodableValue(7)}};

  EXPECT_TRUE(FingerprintRecords({no_id, number_id}).empty());
}

// Purpose: Rewriting a media file changes its fingerprint, so media
// observers see modifications and not only additions and removals.
TEST(FingerprintRecordsTest, ChangesWhenAMediaFileIsRewritten) {
  TempDirectory directory;
  const auto photo = directory.WriteFile("photo.jpg");
  const auto list_media = [&directory]() {
    return FingerprintRecords(
        RowsOrThrow(ListRecords({.root = directory.path(), .media_only = true})));
  };
  const Snapshot before = list_media();

  std::filesystem::last_write_time(photo,
                                   std::filesystem::last_write_time(photo) + 1h);

  ASSERT_EQ(before.size(), 1u);
  EXPECT_NE(before.at(PathToUtf8(photo)), list_media().at(PathToUtf8(photo)));
}


// Purpose: Changing a value inside a nested list changes the fingerprint,
// so a contact whose second phone number changes is reported.
TEST(FingerprintRecordsTest, ChangesWhenANestedListChanges) {
  const EncodableMap before{
      {EncodableValue("id"), EncodableValue("a")},
      {EncodableValue("phones"),
       EncodableValue(EncodableList{EncodableValue("111"), EncodableValue("222")})}};
  EncodableMap after = before;
  after[EncodableValue("phones")] =
      EncodableValue(EncodableList{EncodableValue("111"), EncodableValue("333")});

  EXPECT_NE(FingerprintRecords({before}).at("a"), FingerprintRecords({after}).at("a"));
}

// Purpose: Values of different types never share a fingerprint, so a field
// that changes from the number 1 to the string "1" is reported.
TEST(FingerprintRecordsTest, ChangesWhenAFieldChangesType) {
  const EncodableMap number{{EncodableValue("id"), EncodableValue("a")},
                            {EncodableValue("value"), EncodableValue(1)}};
  const EncodableMap text{{EncodableValue("id"), EncodableValue("a")},
                          {EncodableValue("value"), EncodableValue("1")}};

  EXPECT_NE(FingerprintRecords({number}).at("a"), FingerprintRecords({text}).at("a"));
}

// Purpose: A custom value cannot be fingerprinted, so it fails the snapshot
// instead of being skipped.
TEST(FingerprintRecordsTest, RejectsACustomValue) {
  const EncodableMap row{{EncodableValue("id"), EncodableValue("a")},
                         {EncodableValue("custom"),
                          EncodableValue(CustomEncodableValue(std::any(7)))}};

  EXPECT_THROW(FingerprintRecords({row}), std::invalid_argument);
}

// Purpose: Read one string field of a row.
// @param row is the row.
// @param key is the field name.
// @returns The field's text.
// @throws std::out_of_range when the field is absent, std::bad_variant_access
// when it is not a string.
std::string TextField(const EncodableMap& row, const std::string& key) {
  return std::get<std::string>(row.at(EncodableValue(key)));
}

// Purpose: Without a stop check the walk lists every file.
TEST(ListRecordsTest, ListsEveryFileWithoutAStopCheck) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }

  const RowsResult result = ListRecords({.root = directory.path()});

  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(result.rows.size(), 5u);
}

// Purpose: The walk ends as soon as its stop check turns true, and reports
// the stop, so a partial walk is never taken for a complete one.
TEST(ListRecordsTest, ReportsAStopInsteadOfAPartialList) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }
  int checks = 0;
  // Checked once before each entry, so the third check ends the walk.
  const RowsResult result = ListRecords(
      {.root = directory.path(), .stop_requested = [&checks]() { return ++checks > 2; }});

  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, kReadStoppedError);
  EXPECT_EQ(checks, 3);
}

// Purpose: A walk asked to stop before it starts reports the stop.
TEST(ListRecordsTest, ReportsAStopBeforeTheFirstEntry) {
  TempDirectory directory;
  directory.WriteFile("file.txt");

  const RowsResult result =
      ListRecords({.root = directory.path(), .stop_requested = []() { return true; }});

  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, kReadStoppedError);
}

// Purpose: A root that does not exist holds no files, which is not an error.
TEST(ListRecordsTest, ListsNoRowsForAMissingRoot) {
  TempDirectory directory;

  const RowsResult result = ListRecords({.root = directory.path() / "missing"});

  EXPECT_FALSE(result.error.has_value());
  EXPECT_TRUE(result.rows.empty());
}

// Purpose: A root that cannot be walked fails the read instead of listing
// nothing, so a query never reports an unreadable tree as empty.
TEST(ListRecordsTest, FailsWhenTheRootCannotBeWalked) {
  TempDirectory directory;
  const auto file_root = directory.WriteFile("not_a_directory.txt");

  const RowsResult result = ListRecords({.root = file_root});

  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("filesystem query failed"), std::string::npos)
      << *result.error;
}

// Purpose: Times are ISO 8601 UTC, and the creation and modification times
// are read separately.
TEST(ListRecordsTest, ReportsSeparateIsoCreationAndModificationTimes) {
  TempDirectory directory;
  const auto photo = directory.WriteFile("photo.jpg");
  std::filesystem::last_write_time(photo,
                                   std::filesystem::last_write_time(photo) + 1h);

  const RowsResult result = ListRecords({.root = directory.path(), .media_only = true});

  ASSERT_FALSE(result.error.has_value()) << *result.error;
  ASSERT_EQ(result.rows.size(), 1u);
  const std::string created = TextField(result.rows[0], "createdAt");
  const std::string modified = TextField(result.rows[0], "modifiedAt");
  EXPECT_TRUE(IsIsoUtc(created)) << created;
  EXPECT_TRUE(IsIsoUtc(modified)) << modified;
  EXPECT_NE(created, modified);
}

// Purpose: modifiedEpochMs counts from 1970, not from the 1601 epoch MSVC's
// file clock uses, so a file written now reads as now.
TEST(ListRecordsTest, ReportsModificationTimeFromTheUnixEpoch) {
  TempDirectory directory;
  directory.WriteFile("notes.txt");
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

  const RowsResult result = ListRecords({.root = directory.path()});

  ASSERT_FALSE(result.error.has_value()) << *result.error;
  ASSERT_EQ(result.rows.size(), 1u);
  const int64_t modified_ms =
      std::get<int64_t>(result.rows[0].at(EncodableValue("modifiedEpochMs")));
  const int64_t one_day_ms = 24LL * 60 * 60 * 1000;
  EXPECT_LT(std::llabs(modified_ms - now_ms), one_day_ms);
}

// Purpose: A name outside ASCII reaches Dart as UTF-8, whatever the system
// code page.
TEST(ListRecordsTest, ReportsANonAsciiNameAsUtf8) {
  TempDirectory directory;
  const std::string utf8_name = "caf\xC3\xA9.txt";
  std::ofstream(directory.path() / PathFromUtf8(utf8_name)) << "changed";

  const RowsResult result = ListRecords({.root = directory.path()});

  ASSERT_FALSE(result.error.has_value()) << *result.error;
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(TextField(result.rows[0], "name"), utf8_name);
  EXPECT_EQ(TextField(result.rows[0], "path"),
            PathToUtf8(directory.path() / PathFromUtf8(utf8_name)));
}

// Purpose: UTF-8 text and a path convert to each other without loss.
TEST(BoundaryFormatsTest, RoundTripsAUtf8Path) {
  const std::string utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE8\xA8\x98.txt";

  EXPECT_EQ(PathToUtf8(PathFromUtf8(utf8)), utf8);
}

// Purpose: An instant formats as ISO 8601 UTC with milliseconds.
TEST(BoundaryFormatsTest, FormatsAnInstantAsIsoUtc) {
  const std::chrono::system_clock::time_point instant(
      std::chrono::milliseconds(1700000000123));

  EXPECT_EQ(FormatIsoUtc(instant), "2023-11-14T22:13:20.123Z");
}

// Purpose: Windows ticks convert to the Unix epoch, so the Windows epoch
// offset of 1601 to 1970 is removed.
TEST(BoundaryFormatsTest, ConvertsWindowsTicksToTheUnixEpoch) {
  constexpr int64_t kUnixEpochInWindowsTicks = 116444736000000000;
  constexpr int64_t kTicksPerMillisecond = 10000;

  EXPECT_EQ(TimeFromWindowsTicks(kUnixEpochInWindowsTicks),
            std::chrono::system_clock::time_point());
  EXPECT_EQ(TimeFromWindowsTicks(kUnixEpochInWindowsTicks + 1500 * kTicksPerMillisecond),
            std::chrono::system_clock::time_point(std::chrono::milliseconds(1500)));
}

// Purpose: A read without an error gives its rows.
TEST(RowsOrThrowTest, GivesTheRowsOfASuccessfulRead) {
  RowsResult result;
  result.rows.push_back(EncodableMap{{EncodableValue("id"), EncodableValue("a")}});

  const auto rows = RowsOrThrow(std::move(result));

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(TextField(rows[0], "id"), "a");
}

// Purpose: A read that had an error fails, carrying that error, so it is
// never mistaken for an empty store.
TEST(RowsOrThrowTest, FailsAReadThatHadAnError) {
  RowsResult result;
  result.rows.push_back(EncodableMap{{EncodableValue("id"), EncodableValue("a")}});
  result.error = "store unavailable";

  try {
    RowsOrThrow(std::move(result));
    ADD_FAILURE() << "RowsOrThrow returned rows from a failed read";
  } catch (const ReadDeniedError&) {
    ADD_FAILURE() << "a read that was not denied failed as denied";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "store unavailable");
  }
}

// Purpose: A denied read fails as denied, carrying the permission id, so
// callers can report permission-denied.
TEST(RowsOrThrowTest, FailsADeniedReadWithItsPermission) {
  RowsResult result;
  result.error = "contacts access is denied";
  result.denied_capability = "contacts";

  try {
    RowsOrThrow(std::move(result));
    ADD_FAILURE() << "RowsOrThrow returned rows from a denied read";
  } catch (const ReadDeniedError& error) {
    EXPECT_STREQ(error.what(), "contacts access is denied");
    EXPECT_EQ(error.capability(), "contacts");
  }
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
