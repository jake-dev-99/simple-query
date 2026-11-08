#include "query_shaping.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "file_records.h"
#include "request_values.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Parse a filter operator name Dart sends.
// @param name is the QueryFilterOperator name.
// @returns The operator, or nothing for a name Windows does not know.
// @throws Nothing.
std::optional<FilterOperator> ParseOperator(const std::string& name) {
  if (name == "equals") return FilterOperator::kEquals;
  if (name == "notEquals") return FilterOperator::kNotEquals;
  if (name == "greaterThan") return FilterOperator::kGreaterThan;
  if (name == "greaterThanOrEqual") return FilterOperator::kGreaterThanOrEqual;
  if (name == "lessThan") return FilterOperator::kLessThan;
  if (name == "lessThanOrEqual") return FilterOperator::kLessThanOrEqual;
  if (name == "contains") return FilterOperator::kContains;
  if (name == "inList") return FilterOperator::kInList;
  return std::nullopt;
}

// Purpose: Check whether a value is a double that is not a number.
// @param value may hold any type; only a double can be NaN.
// @returns Whether the value is NaN.
// @throws Nothing.
bool IsNan(const EncodableValue& value) {
  const auto* number = std::get_if<double>(&value);
  return number != nullptr && std::isnan(*number);
}

// Purpose: Check a filter value, or any item of a list value, for NaN. NaN
// has no order, so a NaN filter could match every row of a filtered delete.
// @param value is the filter value, or null when the filter has none.
// @returns Whether the value holds NaN.
// @throws Nothing.
bool HoldsNan(const EncodableValue* value) {
  if (const auto* items = AsList(value)) {
    return std::any_of(items->begin(), items->end(), IsNan);
  }
  return value != nullptr && IsNan(*value);
}

// Purpose: Check whether a filter lacks the field or operator it needs.
// @param field is the parsed field name, if present.
// @param op_name is the parsed operator name, if present.
// @returns Whether the filter is incomplete.
// @throws Nothing.
bool IsIncompleteFilter(const std::optional<std::string>& field,
                        const std::optional<std::string>& op_name) {
  return !field.has_value() || field->empty() || !op_name.has_value();
}

// Purpose: Parse one filter map of a request.
// @param value is the filter value.
// @returns The filter, or the reason it is invalid.
// @throws Nothing.
std::variant<QueryFilter, std::string> ParseFilter(const EncodableValue& value) {
  const auto* filter = std::get_if<EncodableMap>(&value);
  if (filter == nullptr) {
    return std::string("simple_query: each filter must be a map");
  }
  const auto field = StringAt(*filter, "field");
  const auto op_name = StringAt(*filter, "operator");
  if (IsIncompleteFilter(field, op_name)) {
    return std::string("simple_query: each filter needs a field and an operator");
  }
  const auto op = ParseOperator(*op_name);
  if (!op.has_value()) {
    return "simple_query: filter operator " + *op_name + " is not supported on Windows";
  }
  const auto* expected = FindValue(*filter, "value");
  if (*op == FilterOperator::kInList && AsList(expected) == nullptr) {
    return std::string("simple_query: the inList filter needs a list value");
  }
  if (HoldsNan(expected)) {
    return std::string("simple_query: a filter value cannot be NaN. Send a number.");
  }
  return QueryFilter{.field = *field,
                     .op = *op,
                     .value = expected == nullptr ? std::nullopt
                                                  : std::optional<EncodableValue>(*expected)};
}

// Purpose: Order two values that are both integers.
// @param lhs is the first value.
// @param rhs is the second value.
// @returns -1, 0 or 1, or nothing when either is not an integer.
// @throws Nothing.
std::optional<int> CompareIntegers(const EncodableValue& lhs, const EncodableValue& rhs) {
  const bool lhs_integer =
      std::holds_alternative<int32_t>(lhs) || std::holds_alternative<int64_t>(lhs);
  const bool rhs_integer =
      std::holds_alternative<int32_t>(rhs) || std::holds_alternative<int64_t>(rhs);
  if (!lhs_integer || !rhs_integer) {
    return std::nullopt;
  }
  const int64_t left = *AsInt(&lhs);
  const int64_t right = *AsInt(&rhs);
  return (left > right) - (left < right);
}

// Purpose: Read a number as a double.
// @param value may hold any type; an integer is widened to a double.
// @returns The number, or nothing when the value is not a number.
// @throws Nothing.
std::optional<double> AsDouble(const EncodableValue& value) {
  if (const auto* number = std::get_if<double>(&value)) {
    return *number;
  }
  const auto integer = AsInt(&value);
  if (integer.has_value()) {
    return static_cast<double>(*integer);
  }
  return std::nullopt;
}

// Purpose: Report whether an operand cannot be ordered because it is
// absent or NaN.
// @param value is the operand, or null when its field is absent.
// @returns true when the operand is absent or NaN.
// @throws Nothing.
bool IsAbsentOrUnordered(const EncodableValue* value) {
  return value == nullptr || IsNan(*value);
}

// Purpose: Order two field values: integers exactly, other numbers as
// doubles, and any other scalars as text.
// @param lhs is the first value, or null when the field is absent.
// @param rhs is the second value, or null when the field is absent.
// @returns -1, 0 or 1, or nothing when either value is absent, NaN or not
// a scalar.
// @throws Nothing.
std::optional<int> CompareValues(const EncodableValue* lhs, const EncodableValue* rhs) {
  // NaN is unordered: its < and > are both false, which would read as equal.
  if (IsAbsentOrUnordered(lhs) || IsAbsentOrUnordered(rhs)) {
    return std::nullopt;
  }
  if (const auto integers = CompareIntegers(*lhs, *rhs); integers.has_value()) {
    return integers;
  }
  const auto left_number = AsDouble(*lhs);
  const auto right_number = AsDouble(*rhs);
  if (left_number.has_value() && right_number.has_value()) {
    return (*left_number > *right_number) - (*left_number < *right_number);
  }
  const auto left_text = AsString(lhs);
  const auto right_text = AsString(rhs);
  if (!left_text.has_value() || !right_text.has_value()) {
    return std::nullopt;
  }
  const int order = left_text->compare(*right_text);
  return (order > 0) - (order < 0);
}

// Purpose: Check two values for equality the way Dart's == does for filter
// values: null equals only null, and other values compare as scalars. An
// absent field counts as null, because Dart reads a missing field as null.
// @param lhs is the first value, or null when the field is absent.
// @param rhs is the second value, or null when the field is absent.
// @returns Whether the values are equal.
// @throws Nothing.
bool ValuesEqual(const EncodableValue* lhs, const EncodableValue* rhs) {
  if (IsAbsentOrNull(lhs) || IsAbsentOrNull(rhs)) {
    return IsAbsentOrNull(lhs) && IsAbsentOrNull(rhs);
  }
  return CompareValues(lhs, rhs) == 0;
}

// Purpose: Check a contains filter: a list field holds the value, or a text
// field holds it as a substring, ignoring ASCII case.
// @param actual is the field value, or null when the field is absent.
// @param expected is the filter value.
// @returns Whether the field contains the value.
// @throws Nothing.
bool Contains(const EncodableValue* actual, const EncodableValue* expected) {
  if (const auto* items = AsList(actual)) {
    return std::any_of(items->begin(), items->end(), [&](const EncodableValue& item) {
      return ValuesEqual(&item, expected);
    });
  }
  const auto haystack = AsString(actual);
  const auto needle = AsString(expected);
  if (!haystack.has_value() || !needle.has_value()) {
    return false;
  }
  return Lower(*haystack).find(Lower(*needle)) != std::string::npos;
}

// Purpose: Check one row against one filter.
// @param row is the row.
// @param filter is the parsed filter.
// @returns Whether the row passes.
// @throws Nothing.
bool Passes(const EncodableMap& row, const QueryFilter& filter) {
  const auto* actual = FindValue(row, filter.field);
  const EncodableValue* expected = filter.value.has_value() ? &*filter.value : nullptr;
  if (filter.op == FilterOperator::kContains) {
    return Contains(actual, expected);
  }
  if (filter.op == FilterOperator::kInList) {
    const auto* options = AsList(expected);
    return std::any_of(options->begin(), options->end(), [&](const EncodableValue& option) {
      return ValuesEqual(actual, &option);
    });
  }
  if (filter.op == FilterOperator::kEquals) {
    return ValuesEqual(actual, expected);
  }
  if (filter.op == FilterOperator::kNotEquals) {
    return !ValuesEqual(actual, expected);
  }
  const auto order = CompareValues(actual, expected);
  switch (filter.op) {
    case FilterOperator::kGreaterThan:
      return order.has_value() && *order > 0;
    case FilterOperator::kGreaterThanOrEqual:
      return order.has_value() && *order >= 0;
    case FilterOperator::kLessThan:
      return order.has_value() && *order < 0;
    case FilterOperator::kLessThanOrEqual:
      return order.has_value() && *order <= 0;
    case FilterOperator::kEquals:
    case FilterOperator::kNotEquals:
    case FilterOperator::kContains:
    case FilterOperator::kInList:
      break;
  }
  return false;
}

// Purpose: Parse the sort key of a request; only the first key is used.
// @param request is the query request.
// @returns The sort key, nothing when the request has none, or the reason
// it is invalid.
// @throws Nothing.
std::variant<std::optional<QuerySort>, std::string> ParseSort(const EncodableMap& request) {
  const auto* sort = ListAt(request, "sort");
  if (sort == nullptr || sort->empty()) {
    return std::optional<QuerySort>{};
  }
  const auto* first = std::get_if<EncodableMap>(&sort->front());
  const auto field = first == nullptr ? std::nullopt : StringAt(*first, "field");
  if (!field.has_value() || field->empty()) {
    return std::string("simple_query: each sort key needs a field");
  }
  const auto direction = StringAt(*first, "direction");
  const bool known_direction =
      !direction.has_value() || *direction == "ascending" || *direction == "descending";
  if (!known_direction) {
    return "simple_query: sort direction " + *direction + " is not supported";
  }
  return std::optional<QuerySort>(
      QuerySort{.field = *field, .ascending = direction != "descending"});
}

// Purpose: Parse the projection of a request.
// @param request is the query request.
// @returns The field names, empty for every field, or the reason the
// projection is invalid.
// @throws Nothing.
std::variant<std::vector<std::string>, std::string> ParseProjection(
    const EncodableMap& request) {
  std::vector<std::string> fields;
  const auto* projection = ListAt(request, "projection");
  if (projection == nullptr) {
    return fields;
  }
  for (const auto& value : *projection) {
    const auto* field = std::get_if<std::string>(&value);
    if (field == nullptr) {
      return std::string("simple_query: each projection field must be text");
    }
    fields.push_back(*field);
  }
  return fields;
}

// Purpose: Parse one number of a request's page.
// @param page is the request's page map.
// @param key is offset or limit.
// @returns The number, nothing when the page omits it, or the reason it is
// invalid.
// @throws Nothing.
std::variant<std::optional<int64_t>, std::string> ParsePageNumber(const EncodableMap& page,
                                                                  const std::string& key) {
  const EncodableValue* value = FindValue(page, key);
  if (IsAbsentOrNull(value)) {
    return std::optional<int64_t>{};
  }
  const std::optional<int64_t> number = AsInt(value);
  if (!number.has_value()) {
    return "simple_query: page." + key + " must be a whole number. Send an integer.";
  }
  return number;
}

// Purpose: Rank a sort value's kind, so values of different kinds still
// sort in one consistent order.
// @param value is the field value, or null when the field is absent.
// @returns 0 for absent or null, 1 for a number, 2 for NaN, 3 for text or
// a bool, and 4 for a list or map.
// @throws Nothing.
int SortRank(const EncodableValue* value) {
  if (IsAbsentOrNull(value)) {
    return 0;
  }
  // NaN ranks after every number, as Dart's num.compareTo orders it. Ranking
  // it as a number would tie it with every number and break the sort order.
  if (IsNan(*value)) {
    return 2;
  }
  if (AsDouble(*value).has_value()) {
    return 1;
  }
  return AsString(value).has_value() ? 3 : 4;
}

// Purpose: Order two rows' values of the sort field. Absent values come
// first, then numbers, then NaN, then text; lists and maps compare equal.
// @param lhs is the first value, or null when the field is absent.
// @param rhs is the second value, or null when the field is absent.
// @returns A negative, zero or positive order.
// @throws Nothing.
int SortOrder(const EncodableValue* lhs, const EncodableValue* rhs) {
  const int left_rank = SortRank(lhs);
  const int right_rank = SortRank(rhs);
  if (left_rank != right_rank) {
    return left_rank - right_rank;
  }
  // Only two absent values, two NaNs, or two lists or maps have no order;
  // they tie.
  return CompareValues(lhs, rhs).value_or(0);
}

// Purpose: Hold the rows of one page, as a half-open index range.
// @param start is the first kept row's index, at most the row count.
// @param end is one past the last kept row, so the page is [start, end).
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PageBounds {
  size_t start = 0;
  size_t end = 0;
};

// Purpose: Find which rows one page covers.
// @param row_count is the number of rows after filtering.
// @param shape holds the page's offset and limit.
// @returns The page's range; a negative offset or limit counts as zero.
// @throws Nothing.
PageBounds ResolvePage(size_t row_count, const QueryShape& shape) {
  const size_t offset = shape.offset <= 0 ? 0 : static_cast<size_t>(shape.offset);
  PageBounds bounds{.start = std::min(row_count, offset), .end = row_count};
  if (shape.limit.has_value()) {
    const size_t limit = *shape.limit <= 0 ? 0 : static_cast<size_t>(*shape.limit);
    bounds.end = bounds.start + std::min(limit, row_count - bounds.start);
  }
  return bounds;
}

// Purpose: Keep only the projected fields of one row.
// @param row is the row.
// @param projection lists the field names; empty keeps every field.
// @returns The projected row; a missing field is present as null.
// @throws Nothing.
EncodableMap Project(EncodableMap row, const std::vector<std::string>& projection) {
  if (projection.empty()) {
    return row;
  }
  EncodableMap projected;
  for (const auto& field : projection) {
    const auto* value = FindValue(row, field);
    projected[EncodableValue(field)] = value == nullptr ? EncodableValue() : *value;
  }
  return projected;
}

}  // namespace

// Purpose: Parse and check the filters of a request.
// @param request is the query or mutation request.
// @returns The filters, or the reason they are invalid.
// @throws Nothing.
std::variant<std::vector<QueryFilter>, std::string> ParseFilters(const EncodableMap& request) {
  std::vector<QueryFilter> filters;
  const auto* raw_filters = FindValue(request, "filters");
  if (IsAbsentOrNull(raw_filters)) {
    return filters;
  }
  const auto* values = AsList(raw_filters);
  if (values == nullptr) {
    return std::string("simple_query: filters must be a list");
  }
  for (const auto& value : *values) {
    auto parsed = ParseFilter(value);
    if (auto* error = std::get_if<std::string>(&parsed)) {
      return std::move(*error);
    }
    filters.push_back(std::get<QueryFilter>(std::move(parsed)));
  }
  return filters;
}

// Purpose: Parse and check how a query request shapes its rows.
// @param request is the query request.
// @returns The shape, or the reason it is invalid.
// @throws Nothing.
std::variant<QueryShape, std::string> ParseQueryShape(const EncodableMap& request) {
  QueryShape shape;
  auto filters = ParseFilters(request);
  if (auto* error = std::get_if<std::string>(&filters)) {
    return std::move(*error);
  }
  shape.filters = std::get<std::vector<QueryFilter>>(std::move(filters));
  auto sort = ParseSort(request);
  if (auto* error = std::get_if<std::string>(&sort)) {
    return std::move(*error);
  }
  shape.sort = std::get<std::optional<QuerySort>>(std::move(sort));
  auto projection = ParseProjection(request);
  if (auto* error = std::get_if<std::string>(&projection)) {
    return std::move(*error);
  }
  shape.projection = std::get<std::vector<std::string>>(std::move(projection));
  if (const auto* page = MapAt(request, "page")) {
    auto offset = ParsePageNumber(*page, "offset");
    if (auto* error = std::get_if<std::string>(&offset)) {
      return std::move(*error);
    }
    auto limit = ParsePageNumber(*page, "limit");
    if (auto* error = std::get_if<std::string>(&limit)) {
      return std::move(*error);
    }
    // A page without an offset starts at the first row.
    shape.offset = std::get<std::optional<int64_t>>(offset).value_or(0);
    shape.limit = std::get<std::optional<int64_t>>(limit);
  }
  return shape;
}

// Purpose: Keep the rows that pass every filter.
// @param rows are the rows to filter.
// @param filters are the parsed filters.
// @returns The rows that pass, in their original order.
// @throws Nothing.
std::vector<EncodableMap> ApplyFilters(std::vector<EncodableMap> rows,
                                       const std::vector<QueryFilter>& filters) {
  std::vector<EncodableMap> kept;
  for (auto& row : rows) {
    const bool passes = std::all_of(filters.begin(), filters.end(),
                                    [&](const QueryFilter& filter) { return Passes(row, filter); });
    if (passes) {
      kept.push_back(std::move(row));
    }
  }
  return kept;
}

// Purpose: Filter, sort, page and project rows into a query result.
// @param rows are every row of the domain.
// @param shape is the parsed query shape.
// @returns The result map Dart reads: records, totalCount and, when more
// rows follow, nextOffset.
// @throws Nothing.
EncodableMap ShapeQueryResult(std::vector<EncodableMap> rows, const QueryShape& shape) {
  rows = ApplyFilters(std::move(rows), shape.filters);
  if (shape.sort.has_value()) {
    const QuerySort& sort = *shape.sort;
    // Stable, so rows that compare equal keep their store order.
    std::stable_sort(rows.begin(), rows.end(), [&](const EncodableMap& lhs, const EncodableMap& rhs) {
      const int order = SortOrder(FindValue(lhs, sort.field), FindValue(rhs, sort.field));
      return sort.ascending ? order < 0 : order > 0;
    });
  }

  const PageBounds page = ResolvePage(rows.size(), shape);
  EncodableList records;
  for (size_t index = page.start; index < page.end; ++index) {
    records.push_back(EncodableValue(Project(std::move(rows[index]), shape.projection)));
  }

  EncodableMap result;
  result[EncodableValue("records")] = EncodableValue(std::move(records));
  result[EncodableValue("totalCount")] = EncodableValue(static_cast<int64_t>(rows.size()));
  if (page.end < rows.size()) {
    result[EncodableValue("nextOffset")] = EncodableValue(static_cast<int64_t>(page.end));
  }
  return result;
}

}  // namespace simple_query_windows
