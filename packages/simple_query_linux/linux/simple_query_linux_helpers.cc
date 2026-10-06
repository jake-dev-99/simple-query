#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

#include "simple_query_linux_plugin_private.h"

namespace simple_query_linux {

/** Purpose: Normalize ASCII metadata for case-insensitive matching.
 * @param value is copied for in-place normalization. @returns Lowercase text.
 * @throws Nothing after the input copy succeeds. */
std::string Lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

/**
 * Purpose: Release a uniquely owned Flutter value through its C API.
 * @param value is the nullable Flutter value leaving scope.
 * @returns Nothing.
 * @throws Nothing.
 */
void FlValueDeleter::operator()(FlValue* value) const {
  if (value != nullptr) {
    fl_value_unref(value);
  }
}

/** Purpose: Adopt an owned Flutter value. @param value is transferred to the
 * returned pointer. @returns An RAII Flutter value. @throws Nothing. */
ValuePtr Value(FlValue* value) { return ValuePtr(value); }

/** Purpose: Transfer a value into a string-keyed Flutter map. @param map is
 * mutated. @param key identifies the entry. @param value is transferred.
 * @returns Nothing. @throws Nothing. */
void MapSet(FlValue* map, const char* key, FlValue* value) {
  fl_value_set_string_take(map, key, value);
}

/** Purpose: Store a copied string in a Flutter map. @param map is mutated.
 * @param key identifies the entry. @param value supplies the string.
 * @returns Nothing. @throws Nothing. */
void MapSetString(FlValue* map, const char* key, const std::string& value) {
  MapSet(map, key, fl_value_new_string(value.c_str()));
}

/** Purpose: Store a Boolean in a Flutter map. @param map is mutated. @param key
 * identifies the entry. @param value supplies the Boolean. @returns Nothing.
 * @throws Nothing. */
void MapSetBool(FlValue* map, const char* key, bool value) {
  MapSet(map, key, fl_value_new_bool(value));
}

/** Purpose: Store an integer in a Flutter map. @param map is mutated. @param
 * key identifies the entry. @param value supplies the integer. @returns
 * Nothing.
 * @throws Nothing. */
void MapSetInt(FlValue* map, const char* key, int64_t value) {
  MapSet(map, key, fl_value_new_int(value));
}

/** Purpose: Safely look up a string key only when the input is a map.
 * @param map is the nullable candidate map. @param key identifies the entry.
 * @returns A borrowed value or null. @throws Nothing. */
FlValue* FindValue(FlValue* map, const std::string& key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }
  return fl_value_lookup_string(map, key.c_str());
}

/** Purpose: Normalize scalar Flutter values to text for query comparison.
 * @param value is the nullable scalar. @returns Text or null for unsupported
 * types. @throws std::bad_alloc when text allocation fails. */
std::optional<std::string> AsString(FlValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_STRING:
      return std::string(fl_value_get_string(value));
    case FL_VALUE_TYPE_INT:
      return std::to_string(fl_value_get_int(value));
    case FL_VALUE_TYPE_FLOAT:
      return std::to_string(fl_value_get_float(value));
    case FL_VALUE_TYPE_BOOL:
      return fl_value_get_bool(value) ? "true" : "false";
    default:
      return std::nullopt;
  }
}

/** Purpose: Read a map string while preserving an explicit fallback.
 * @param map supplies the value. @param key identifies it. @param fallback is
 * used when conversion fails. @returns The converted or fallback text.
 * @throws std::bad_alloc when text allocation fails. */
std::string StringOr(FlValue* map, const std::string& key,
                     const std::string& fallback) {
  return AsString(FindValue(map, key)).value_or(fallback);
}

/** Purpose: Normalize numeric Flutter values to a signed integer.
 * @param value is the nullable scalar. @returns An integer or null for an
 * unsupported type. @throws Nothing. */
std::optional<int64_t> AsInt(FlValue* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_INT:
      return fl_value_get_int(value);
    case FL_VALUE_TYPE_FLOAT:
      return static_cast<int64_t>(fl_value_get_float(value));
    default:
      return std::nullopt;
  }
}

/** Purpose: Read a strict Boolean map field without coercion. @param map
 * supplies the value. @param key identifies it. @param fallback is used when
 * absent or mistyped. @returns The decoded or fallback Boolean.
 * @throws Nothing. */
bool BoolOr(FlValue* map, const std::string& key, bool fallback) {
  FlValue* value = FindValue(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
    return fallback;
  }
  return fl_value_get_bool(value);
}

/** Purpose: Validate a Flutter value as a map. @param value is borrowed.
 * @returns The same borrowed value or null. @throws Nothing. */
FlValue* AsMap(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_MAP
             ? value
             : nullptr;
}

/** Purpose: Validate a Flutter value as a list. @param value is borrowed.
 * @returns The same borrowed value or null. @throws Nothing. */
FlValue* AsList(FlValue* value) {
  return value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_LIST
             ? value
             : nullptr;
}

/** Purpose: Copy a Flutter map container while retaining each mapped value.
 * @param map is borrowed and may be null or mistyped. @returns A new map.
 * @throws Nothing. */
ValuePtr CloneMap(FlValue* map) {
  auto clone = Value(fl_value_new_map());
  if (AsMap(map) == nullptr) {
    return clone;
  }
  for (size_t index = 0; index < fl_value_get_length(map); index++) {
    FlValue* key = fl_value_get_map_key(map, index);
    if (fl_value_get_type(key) != FL_VALUE_TYPE_STRING) {
      continue;
    }
    MapSet(clone.get(), fl_value_get_string(key),
           fl_value_ref(fl_value_get_map_value(map, index)));
  }
  return clone;
}

/**
 * Purpose: Build the stable unavailable detail used for filesystem failures.
 * @param operation describes the failed filesystem action.
 * @param path identifies the record that could not be inspected.
 * @param error contains the operating-system failure.
 * @returns A simple_query-prefixed diagnostic safe to cross the host boundary.
 * @throws std::bad_alloc when diagnostic allocation fails.
 */
std::string FileSystemFailure(const char* operation,
                              const std::filesystem::path& path,
                              const std::error_code& error) {
  return "simple_query: " + std::string(operation) + " failed for " +
         path.string() + " - " + error.message();
}

/** Purpose: Read a record field as comparable text. @param row is the record.
 * @param key selects its field. @returns Converted text or an empty string.
 * @throws std::bad_alloc when text allocation fails. */
std::string ValueAsString(FlValue* row, const std::string& key) {
  return AsString(FindValue(row, key)).value_or("");
}

/** Purpose: Encode one runtime capability descriptor. @param domain names the
 * domain. @param can_read permits reads. @param can_write permits writes.
 * @param can_observe permits observation. @param can_stream permits binary
 * streams. @param reason explains a restriction. @returns A new descriptor.
 * @throws Nothing. */
ValuePtr Capability(const std::string& domain, bool can_read, bool can_write,
                    bool can_observe, bool can_stream,
                    const std::optional<std::string>& reason) {
  auto map = Value(fl_value_new_map());
  MapSetString(map.get(), "domain", domain);
  MapSetBool(map.get(), "canRead", can_read);
  MapSetBool(map.get(), "canWrite", can_write);
  MapSetBool(map.get(), "canObserve", can_observe);
  MapSetBool(map.get(), "canStream", can_stream);
  if (reason.has_value()) {
    MapSetString(map.get(), "reason", *reason);
  }
  return map;
}

/** Purpose: Resolve a requested filesystem root or the current directory.
 * @param request may contain platformData.rootPath. @returns The selected path.
 * @throws std::filesystem::filesystem_error when current-directory lookup
 * fails, and std::bad_alloc on path allocation. */
std::filesystem::path ResolveRootPath(FlValue* request) {
  FlValue* platform_data = AsMap(FindValue(request, "platformData"));
  if (platform_data != nullptr) {
    const auto root = AsString(FindValue(platform_data, "rootPath"));
    if (root.has_value() && !root->empty()) {
      return std::filesystem::path(*root);
    }
  }
  return std::filesystem::current_path();
}

/** Purpose: Apply portable equality, containment, and list filters in order.
 * @param rows owns candidate records. @param filters is the borrowed filter
 * list. @returns The retained records. @throws std::bad_alloc on allocation. */
Rows ApplyFilters(Rows rows, FlValue* filters) {
  if (filters == nullptr || fl_value_get_length(filters) == 0) {
    return rows;
  }

  Rows filtered;
  for (auto& row : rows) {
    bool keep = true;
    for (size_t index = 0; index < fl_value_get_length(filters); index++) {
      FlValue* filter = AsMap(fl_value_get_list_value(filters, index));
      if (filter == nullptr) {
        continue;
      }
      const auto field = AsString(FindValue(filter, "field"));
      const auto operation = AsString(FindValue(filter, "operator"));
      if (!field.has_value() || !operation.has_value()) {
        continue;
      }

      const std::string actual = ValueAsString(row.get(), *field);
      FlValue* expected = FindValue(filter, "value");
      if (*operation == "equals") {
        if (actual != AsString(expected).value_or("")) {
          keep = false;
          break;
        }
      } else if (*operation == "contains") {
        const std::string needle = Lower(AsString(expected).value_or(""));
        if (Lower(actual).find(needle) == std::string::npos) {
          keep = false;
          break;
        }
      } else if (*operation == "inList") {
        FlValue* expected_list = AsList(expected);
        if (expected_list != nullptr) {
          bool matched = false;
          for (size_t item_index = 0;
               item_index < fl_value_get_length(expected_list); item_index++) {
            if (actual ==
                AsString(fl_value_get_list_value(expected_list, item_index))
                    .value_or("")) {
              matched = true;
              break;
            }
          }
          if (!matched) {
            keep = false;
            break;
          }
        }
      }
    }

    if (keep) {
      filtered.push_back(std::move(row));
    }
  }
  return filtered;
}

/** Purpose: Compare one exact integer with a binary floating-point value.
 * @param integer is represented without precision loss.
 * @param floating is finite or nonfinite.
 * @returns Negative, zero, or positive when integer sorts before, with, or
 * after floating; NaN sorts after every other numeric value.
 * @throws Nothing. */
int CompareIntAndFloat(int64_t integer, double floating) {
  if (std::isnan(floating)) return -1;
  if (floating == std::numeric_limits<double>::infinity()) return -1;
  if (floating == -std::numeric_limits<double>::infinity()) return 1;
  constexpr double kTwoTo63 = 9223372036854775808.0;
  if (floating >= kTwoTo63) return -1;
  if (floating < -kTwoTo63) return 1;
  const auto integral_part = static_cast<int64_t>(floating);
  if (integer < integral_part) return -1;
  if (integer > integral_part) return 1;
  const double integral_as_float = static_cast<double>(integral_part);
  if (floating > integral_as_float) return -1;
  if (floating < integral_as_float) return 1;
  return 0;
}

/** Purpose: Compare two numeric Flutter values without losing integer precision.
 * @param left is the first nullable field value.
 * @param right is the second nullable field value.
 * @returns Negative, zero, or positive in stable ascending order, or null when
 * either value is not numeric.
 * @throws Nothing. */
std::optional<int> CompareNumericValues(FlValue* left, FlValue* right) {
  const FlValueType left_type =
      left == nullptr ? FL_VALUE_TYPE_NULL : fl_value_get_type(left);
  const FlValueType right_type =
      right == nullptr ? FL_VALUE_TYPE_NULL : fl_value_get_type(right);
  const bool left_int = left_type == FL_VALUE_TYPE_INT;
  const bool right_int = right_type == FL_VALUE_TYPE_INT;
  const bool left_float = left_type == FL_VALUE_TYPE_FLOAT;
  const bool right_float = right_type == FL_VALUE_TYPE_FLOAT;
  if (left_int && right_int) {
    const int64_t left_value = fl_value_get_int(left);
    const int64_t right_value = fl_value_get_int(right);
    return (left_value > right_value) - (left_value < right_value);
  }
  if (left_int && right_float) {
    return CompareIntAndFloat(fl_value_get_int(left), fl_value_get_float(right));
  }
  if (left_float && right_int) {
    return -CompareIntAndFloat(fl_value_get_int(right),
                               fl_value_get_float(left));
  }
  if (left_float && right_float) {
    const double left_value = fl_value_get_float(left);
    const double right_value = fl_value_get_float(right);
    const bool left_nan = std::isnan(left_value);
    const bool right_nan = std::isnan(right_value);
    if (left_nan || right_nan) {
      return left_nan == right_nan ? 0 : (left_nan ? 1 : -1);
    }
    return (left_value > right_value) - (left_value < right_value);
  }
  return std::nullopt;
}

/** Purpose: Apply the first requested portable sort to native records.
 * @param rows owns records reordered in place. @param sort is the borrowed sort
 * list. @returns Nothing. @throws std::bad_alloc during text comparison. */
void ApplySort(Rows* rows, FlValue* sort) {
  if (sort == nullptr || fl_value_get_length(sort) == 0) {
    return;
  }
  FlValue* first = AsMap(fl_value_get_list_value(sort, 0));
  if (first == nullptr) {
    return;
  }
  const auto field = AsString(FindValue(first, "field"));
  if (!field.has_value()) {
    return;
  }
  const bool ascending =
      AsString(FindValue(first, "direction")).value_or("ascending") !=
      "descending";
  std::sort(rows->begin(), rows->end(),
            [&](const ValuePtr& left, const ValuePtr& right) {
              FlValue* left_value = FindValue(left.get(), *field);
              FlValue* right_value = FindValue(right.get(), *field);
              const auto numeric_comparison =
                  CompareNumericValues(left_value, right_value);
              const bool left_numeric =
                  left_value != nullptr &&
                  (fl_value_get_type(left_value) == FL_VALUE_TYPE_INT ||
                   fl_value_get_type(left_value) == FL_VALUE_TYPE_FLOAT);
              const bool right_numeric =
                  right_value != nullptr &&
                  (fl_value_get_type(right_value) == FL_VALUE_TYPE_INT ||
                   fl_value_get_type(right_value) == FL_VALUE_TYPE_FLOAT);
              int comparison;
              if (numeric_comparison.has_value()) {
                comparison = *numeric_comparison;
              } else if (left_numeric != right_numeric) {
                comparison = left_numeric ? -1 : 1;
              } else {
                comparison = ValueAsString(left.get(), *field)
                                 .compare(ValueAsString(right.get(), *field));
              }
              return ascending ? comparison < 0 : comparison > 0;
            });
}

/** Purpose: Project native rows to the exact requested field set.
 * @param rows supplies source records. @param projection lists field names.
 * @returns A new Flutter list of projected records.
 * @throws std::bad_alloc when projection key conversion fails. */
ValuePtr ApplyProjection(const Rows& rows, FlValue* projection) {
  auto projected_rows = Value(fl_value_new_list());
  if (projection == nullptr || fl_value_get_length(projection) == 0) {
    for (const auto& row : rows) {
      fl_value_append_take(projected_rows.get(), fl_value_ref(row.get()));
    }
    return projected_rows;
  }

  for (const auto& row : rows) {
    auto projected = Value(fl_value_new_map());
    for (size_t index = 0; index < fl_value_get_length(projection); index++) {
      const auto key =
          AsString(fl_value_get_list_value(projection, index)).value_or("");
      FlValue* value = FindValue(row.get(), key);
      MapSet(projected.get(), key.c_str(),
             value != nullptr ? fl_value_ref(value) : fl_value_new_null());
    }
    fl_value_append_take(projected_rows.get(), projected.release());
  }
  return projected_rows;
}

/** Purpose: Wrap an owned Flutter payload as a successful native result.
 * @param value is transferred. @returns A success result. @throws Nothing. */
ValueResult Success(ValuePtr value) {
  return ValueResult{std::move(value), std::nullopt};
}

/** Purpose: Wrap a stable code and message as a native operation failure.
 * @param code is the transport error code. @param message is user-readable.
 * @returns A failed native result. @throws std::bad_alloc while copying text.
 */
ValueResult Failure(const std::string& code, const std::string& message) {
  return ValueResult{ValuePtr(), NativeError{code, message}};
}

}  // namespace simple_query_linux
