// Native tests for record listing and fingerprints: which ids changed
// between snapshots, how a row's fingerprint follows its fields, and the
// file walk.

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <any>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

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
        ListRecords({.root = directory.path(), .media_only = true}));
  };
  const Snapshot before = list_media();

  std::filesystem::last_write_time(photo,
                                   std::filesystem::last_write_time(photo) + 1h);

  ASSERT_EQ(before.size(), 1u);
  EXPECT_NE(before.at(photo.string()), list_media().at(photo.string()));
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

// Purpose: Without a stop check the walk lists every file.
TEST(ListRecordsTest, ListsEveryFileWithoutAStopCheck) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }

  EXPECT_EQ(ListRecords({.root = directory.path()}).size(), 5u);
}

// Purpose: The walk ends as soon as its stop check turns true.
TEST(ListRecordsTest, StopsWalkingOnceAskedTo) {
  TempDirectory directory;
  for (int file = 0; file < 5; ++file) {
    directory.WriteFile("file_" + std::to_string(file) + ".txt");
  }
  int checks = 0;
  // Checked once before each entry, so the third check ends the walk after
  // two entries.
  const auto rows = ListRecords(
      {.root = directory.path(), .stop_requested = [&checks]() { return ++checks > 2; }});

  EXPECT_EQ(rows.size(), 2u);
}

// Purpose: A walk asked to stop before it starts lists nothing.
TEST(ListRecordsTest, ListsNothingWhenAlreadyStopped) {
  TempDirectory directory;
  directory.WriteFile("file.txt");

  EXPECT_TRUE(
      ListRecords({.root = directory.path(), .stop_requested = []() { return true; }})
          .empty());
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
