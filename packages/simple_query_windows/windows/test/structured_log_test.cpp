// Native tests for the plugin's structured log records.

#include "structured_log.h"

#include <gtest/gtest.h>

#include <string>

#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

// Purpose: A record becomes one JSON object: source, level, event, then each
// field in the order given.
// @param None; a test takes no parameters.
// @returns Nothing; a failed expectation fails the test.
// @throws Nothing.
TEST(StructuredLogTest, FormatsTheRecordAsOneJsonObject) {
  const LogRecord record{LogLevel::kError,
                         "observe_snapshot_failed",
                         {{"domain", "contacts"}, {"error", "store unavailable"}}};

  EXPECT_EQ(FormatLogRecord(record),
            "{\"source\":\"simple_query_windows\",\"level\":\"error\","
            "\"event\":\"observe_snapshot_failed\",\"domain\":\"contacts\","
            "\"error\":\"store unavailable\"}");
}

// Purpose: Each level is spelled as the log line expects.
// @param None; a test takes no parameters.
// @returns Nothing; a failed expectation fails the test.
// @throws Nothing.
TEST(StructuredLogTest, NamesEachLevel) {
  const LogRecord warning{LogLevel::kWarning, "event", {}};

  EXPECT_NE(FormatLogRecord(warning).find("\"level\":\"warning\""), std::string::npos);
}

// Purpose: Quotes, backslashes and control characters in a value are escaped,
// so a value can never break the line or the JSON around it.
// @param None; a test takes no parameters.
// @returns Nothing; a failed expectation fails the test.
// @throws Nothing.
TEST(StructuredLogTest, EscapesValuesThatWouldBreakTheLine) {
  const LogRecord record{LogLevel::kError, "event", {{"error", "a \"b\" \\c\nd\te"}}};

  const std::string line = FormatLogRecord(record);

  EXPECT_NE(line.find("\"error\":\"a \\\"b\\\" \\\\c\\u000ad\\u0009e\""),
            std::string::npos);
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

// Purpose: A written record is one line on standard error.
// @param None; a test takes no parameters.
// @returns Nothing; a failed expectation fails the test.
// @throws Nothing.
TEST(StructuredLogTest, WritesOneLineToStandardError) {
  StandardErrorCapture log;

  WriteLog({LogLevel::kWarning, "observe_event_rejected", {{"observer_id", "1"}}});

  const auto lines = log.Lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0],
            "{\"source\":\"simple_query_windows\",\"level\":\"warning\","
            "\"event\":\"observe_event_rejected\",\"observer_id\":\"1\"}");
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
