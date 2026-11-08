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

#include "rows_result.h"

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

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
