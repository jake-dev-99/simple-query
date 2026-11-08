#include "structured_log.h"

#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace simple_query_windows {

namespace {

// Characters below this are control characters, which JSON must escape.
constexpr unsigned char kFirstPrintable = 0x20;

// Purpose: Name a log level as the log line spells it.
// @param level is the level.
// @returns The level's name.
// @throws std::logic_error for a level this function does not know.
const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kWarning:
      return "warning";
    case LogLevel::kError:
      return "error";
  }
  throw std::logic_error("simple_query_windows: a log record has an unknown level");
}

// Purpose: Append text as a quoted JSON string.
// @param text is the text to append.
// @param line is the line being built.
// @returns Nothing.
// @throws std::bad_alloc when the line cannot grow.
void AppendJsonString(const std::string& text, std::string* line) {
  line->push_back('"');
  for (const char character : text) {
    const auto code = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      line->push_back('\\');
      line->push_back(character);
    } else if (code < kFirstPrintable) {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", code);
      line->append(escaped);
    } else {
      line->push_back(character);
    }
  }
  line->push_back('"');
}

// Purpose: Append one "name":"value" member, after a separating comma.
// @param field is the member to append.
// @param line is the line being built.
// @returns Nothing.
// @throws std::bad_alloc when the line cannot grow.
void AppendMember(const LogField& field, std::string* line) {
  line->push_back(',');
  AppendJsonString(field.name, line);
  line->push_back(':');
  AppendJsonString(field.value, line);
}

}  // namespace

std::string FormatLogRecord(const LogRecord& record) {
  std::string line = "{\"source\":\"simple_query_windows\"";
  AppendMember({"level", LevelName(record.level)}, &line);
  AppendMember({"event", record.event}, &line);
  for (const auto& field : record.fields) {
    AppendMember(field, &line);
  }
  line.push_back('}');
  return line;
}

void WriteLog(const LogRecord& record) {
  // One write per line, so lines from two threads never interleave mid-line.
  std::cerr << (FormatLogRecord(record) + "\n");
}

}  // namespace simple_query_windows
