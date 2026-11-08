// Native tests for the host-call work that runs on the background worker:
// how an observer's first snapshot failure reaches Dart.

#include "host_call_work.h"

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "boundary_formats.h"
#include "rows_result.h"
#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

constexpr std::chrono::milliseconds kAnyInterval{1000};

// Purpose: Build an observer spec whose snapshot runs the given step.
// @param take_snapshot is the snapshot step.
// @returns The spec.
// @throws Nothing.
ObserverSpec SpecTaking(std::function<Snapshot(const StopRequested&)> take_snapshot) {
  return {.domain = "contacts",
          .polling_interval = kAnyInterval,
          .take_snapshot = std::move(take_snapshot)};
}

// Purpose: Give a stop check that never asks to stop.
// @returns The stop check.
// @throws Nothing.
StopRequested NeverStop() {
  return []() { return false; };
}

// Purpose: A baseline read the user has not granted fails with
// permission-denied and names the permission, as on Android, so Dart can
// send the user to the right setting.
TEST(HostCallWorkTest, ReportsADeniedBaselineAsPermissionDenied) {
  const BaselineOrError baseline =
      TakeBaseline(SpecTaking([](const StopRequested&) -> Snapshot {
                     throw ReadDeniedError("contacts access is off", "contacts");
                   }),
                   NeverStop());

  ASSERT_TRUE(std::holds_alternative<FlutterError>(baseline));
  const FlutterError& error = std::get<FlutterError>(baseline);
  EXPECT_EQ(error.code(), "permission-denied");
  EXPECT_EQ(error.message(), "contacts access is off");
  const auto& details = std::get<EncodableMap>(error.details());
  EXPECT_EQ(std::get<EncodableList>(details.at(EncodableValue("permissions"))),
            (EncodableList{EncodableValue("contacts")}));
}

// Purpose: Any other baseline failure is unavailable and tells the user
// what to do next.
TEST(HostCallWorkTest, ReportsAFailedBaselineAsUnavailable) {
  const BaselineOrError baseline =
      TakeBaseline(SpecTaking([](const StopRequested&) -> Snapshot {
                     throw std::runtime_error("store offline");
                   }),
                   NeverStop());

  ASSERT_TRUE(std::holds_alternative<FlutterError>(baseline));
  const FlutterError& error = std::get<FlutterError>(baseline);
  EXPECT_EQ(error.code(), "unavailable");
  EXPECT_NE(error.message().find("start again"), std::string::npos);
  EXPECT_NE(error.message().find("store offline"), std::string::npos);
}

// Purpose: A readable baseline is returned unchanged.
TEST(HostCallWorkTest, ReturnsAReadableBaseline) {
  const Snapshot expected{{"a", 1}};

  const BaselineOrError baseline =
      TakeBaseline(SpecTaking([&expected](const StopRequested&) { return expected; }),
                   NeverStop());

  ASSERT_TRUE(std::holds_alternative<Snapshot>(baseline));
  EXPECT_EQ(std::get<Snapshot>(baseline), expected);
}

// Purpose: A directory cannot be consumed by Dart File.readAsBytes, so
// openBinary rejects it before the plugin registers a binary handle.
TEST(HostCallWorkTest, RejectsADirectoryAsABinaryFile) {
  TempDirectory directory;

  const auto result = DescribeBinaryFile(directory.path());

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unavailable");
  EXPECT_NE(result.error().message().find("regular file"), std::string::npos);
}

// Purpose: A regular file keeps its path and byte size for Dart's reader.
TEST(HostCallWorkTest, DescribesARegularBinaryFile) {
  TempDirectory directory;
  const auto path = directory.WriteFile("file.bin");

  const auto result = DescribeBinaryFile(path);

  ASSERT_FALSE(result.has_error()) << result.error().message();
  EXPECT_EQ(std::get<int64_t>(result.value().at(EncodableValue("size"))), 7);
  EXPECT_EQ(std::get<std::string>(result.value().at(EncodableValue("localPath"))),
            PathToUtf8(path));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
