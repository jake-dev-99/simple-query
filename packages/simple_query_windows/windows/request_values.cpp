#include "request_values.h"

#include <cmath>
#include <variant>

namespace simple_query_windows {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Look up one key of a request map.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value, or null when the key is absent.
// @throws Nothing.
const EncodableValue* FindValue(const EncodableMap& map, const std::string& key) {
  const auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &it->second;
}

// Purpose: Check whether a request value is absent or null.
// @param value is the value, or null when the key is absent.
// @returns Whether the value is absent or null.
// @throws Nothing.
bool IsAbsentOrNull(const EncodableValue* value) {
  return value == nullptr || value->IsNull();
}

// Purpose: Read a scalar request value as text.
// @param value is the value, or null.
// @returns The value as text, or nothing when it is absent or not a scalar.
// @throws Nothing.
std::optional<std::string> AsString(const EncodableValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* text = std::get_if<std::string>(value)) {
    return *text;
  }
  if (const auto* i32 = std::get_if<int32_t>(value)) {
    return std::to_string(*i32);
  }
  if (const auto* i64 = std::get_if<int64_t>(value)) {
    return std::to_string(*i64);
  }
  if (const auto* number = std::get_if<double>(value)) {
    return std::to_string(*number);
  }
  if (const auto* flag = std::get_if<bool>(value)) {
    return *flag ? "true" : "false";
  }
  return std::nullopt;
}

// Purpose: Read a numeric request value as an integer.
// @param value is the value, or null.
// @returns The value, truncated when it is a double, or nothing when it is
// absent, not finite, outside int64 range, or not a number.
// @throws Nothing.
std::optional<int64_t> AsInt(const EncodableValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* i32 = std::get_if<int32_t>(value)) {
    return static_cast<int64_t>(*i32);
  }
  if (const auto* i64 = std::get_if<int64_t>(value)) {
    return *i64;
  }
  if (const auto* number = std::get_if<double>(value)) {
    // double(INT64_MAX) is 2^63, so the upper bound must be exclusive.
    const bool safely_representable =
        std::isfinite(*number) && *number >= -0x1p63 && *number < 0x1p63;
    if (!safely_representable) {
      return std::nullopt;
    }
    return static_cast<int64_t>(*number);
  }
  return std::nullopt;
}

// Purpose: View a request value as a map.
// @param value is the value, or null.
// @returns The map, or null when the value is absent or not a map.
// @throws Nothing.
const EncodableMap* AsMap(const EncodableValue* value) {
  return value == nullptr ? nullptr : std::get_if<EncodableMap>(value);
}

// Purpose: View a request value as a list.
// @param value is the value, or null.
// @returns The list, or null when the value is absent or not a list.
// @throws Nothing.
const EncodableList* AsList(const EncodableValue* value) {
  return value == nullptr ? nullptr : std::get_if<EncodableList>(value);
}

// Purpose: Read one key of a map as text.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value as text, or nothing when it is absent or not a scalar.
// @throws Nothing.
std::optional<std::string> StringAt(const EncodableMap& map, const std::string& key) {
  return AsString(FindValue(map, key));
}

// Purpose: Read one key of a map as a bool.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value, or nothing when it is absent or not a bool.
// @throws Nothing.
std::optional<bool> BoolAt(const EncodableMap& map, const std::string& key) {
  const auto* value = FindValue(map, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* flag = std::get_if<bool>(value)) {
    return *flag;
  }
  return std::nullopt;
}

// Purpose: View one key of a map as a map.
// @param map is the map to search.
// @param key is the key's text.
// @returns The map, or null when the key is absent or not a map.
// @throws Nothing.
const EncodableMap* MapAt(const EncodableMap& map, const std::string& key) {
  return AsMap(FindValue(map, key));
}

// Purpose: View one key of a map as a list.
// @param map is the map to search.
// @param key is the key's text.
// @returns The list, or null when the key is absent or not a list.
// @throws Nothing.
const EncodableList* ListAt(const EncodableMap& map, const std::string& key) {
  return AsList(FindValue(map, key));
}

}  // namespace simple_query_windows
