#ifndef FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_STRUCTURED_LOG_H_
#define FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_STRUCTURED_LOG_H_

#include <string>
#include <vector>

namespace simple_query_windows {

// Purpose: The severity of a log record.
enum class LogLevel { kWarning, kError };

// Purpose: One named value in a log record.
struct LogField {
  std::string name;
  std::string value;
};

// Purpose: One log record: its severity, a stable event name, and the
// context needed to reconstruct what happened.
struct LogRecord {
  LogLevel level = LogLevel::kError;
  std::string event;
  std::vector<LogField> fields;
};

// Purpose: Format a record as one line of JSON, with the plugin as its
// source, then the level, the event and each field in order.
// @param record is the record to format.
// @returns The JSON object, without a line ending.
// @throws std::logic_error for a level this formatter does not know.
std::string FormatLogRecord(const LogRecord& record);

// Purpose: Write a record to standard error as one line of JSON.
// @param record is the record to write.
// @returns Nothing.
// @throws std::logic_error for a level this formatter does not know.
void WriteLog(const LogRecord& record);

}  // namespace simple_query_windows

#endif  // FLUTTER_PLUGIN_SIMPLE_QUERY_WINDOWS_STRUCTURED_LOG_H_
