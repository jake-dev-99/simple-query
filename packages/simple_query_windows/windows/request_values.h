#ifndef SIMPLE_QUERY_WINDOWS_REQUEST_VALUES_H_
#define SIMPLE_QUERY_WINDOWS_REQUEST_VALUES_H_

#include <flutter/encodable_value.h>

#include <cstdint>
#include <optional>
#include <string>

namespace simple_query_windows {

// Purpose: Look up one key of a request map.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value, or null when the key is absent.
// @throws Nothing.
const flutter::EncodableValue* FindValue(const flutter::EncodableMap& map,
                                         const std::string& key);

// Purpose: Check whether a request value is absent or null. Dart sends null
// for an unset optional field, so callers treat both the same way.
// @param value is the value, or null when the key is absent.
// @returns Whether the value is absent or null.
// @throws Nothing.
bool IsAbsentOrNull(const flutter::EncodableValue* value);

// Purpose: Read a scalar request value as text.
// @param value is the value, or null.
// @returns The value as text, or nothing when it is absent or not a scalar.
// @throws Nothing.
std::optional<std::string> AsString(const flutter::EncodableValue* value);

// Purpose: Read a numeric request value as an integer.
// @param value is the value, or null.
// @returns The value, truncated when it is a double, or nothing when it is
// absent, not finite, outside int64 range, or not a number.
// @throws Nothing.
std::optional<int64_t> AsInt(const flutter::EncodableValue* value);

// Purpose: View a request value as a map.
// @param value is the value, or null.
// @returns The map, or null when the value is absent or not a map.
// @throws Nothing.
const flutter::EncodableMap* AsMap(const flutter::EncodableValue* value);

// Purpose: View a request value as a list.
// @param value is the value, or null.
// @returns The list, or null when the value is absent or not a list.
// @throws Nothing.
const flutter::EncodableList* AsList(const flutter::EncodableValue* value);

// Purpose: Read one key of a map as text.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value as text, or nothing when it is absent or not a scalar.
// @throws Nothing.
std::optional<std::string> StringAt(const flutter::EncodableMap& map, const std::string& key);

// Purpose: Read one key of a map as a bool.
// @param map is the map to search.
// @param key is the key's text.
// @returns The value, or nothing when it is absent or not a bool.
// @throws Nothing.
std::optional<bool> BoolAt(const flutter::EncodableMap& map, const std::string& key);

// Purpose: View one key of a map as a map.
// @param map is the map to search.
// @param key is the key's text.
// @returns The map, or null when the key is absent or not a map.
// @throws Nothing.
const flutter::EncodableMap* MapAt(const flutter::EncodableMap& map, const std::string& key);

// Purpose: View one key of a map as a list.
// @param map is the map to search.
// @param key is the key's text.
// @returns The list, or null when the key is absent or not a list.
// @throws Nothing.
const flutter::EncodableList* ListAt(const flutter::EncodableMap& map, const std::string& key);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_REQUEST_VALUES_H_
