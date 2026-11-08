#ifndef SIMPLE_QUERY_WINDOWS_QUERY_SHAPING_H_
#define SIMPLE_QUERY_WINDOWS_QUERY_SHAPING_H_

#include <flutter/encodable_value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace simple_query_windows {

// Purpose: Name the comparisons a query filter can make, matching Dart's
// QueryFilterOperator.
enum class FilterOperator {
  kEquals,
  kNotEquals,
  kGreaterThan,
  kGreaterThanOrEqual,
  kLessThan,
  kLessThanOrEqual,
  kContains,
  kInList,
};

// Purpose: Hold one parsed query filter.
// @param field is looked up as one key of the row map.
// @param op is parsed from its wire name, so it is always a known
// operator.
// @param value is the value compared with. It is a copy, not a pointer into
// the request, because the query runs on the background worker after the
// request is gone. Empty when the filter has no value.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct QueryFilter {
  std::string field;
  FilterOperator op = FilterOperator::kEquals;
  std::optional<flutter::EncodableValue> value;
};

// Purpose: Hold the parsed sort key of a query.
// @param field is one key of the row map. Rows without it, or with null,
// sort first in ascending order.
// @param ascending is false only when the request asks for "descending".
// @returns Nothing; this is a data type.
// @throws Nothing.
struct QuerySort {
  std::string field;
  bool ascending = true;
};

// Purpose: Hold how a query filters, sorts, pages and projects its rows,
// parsed and checked once from the request.
// @param filters must all match for a row to stay.
// @param sort holds ordered tie-breakers; when empty, rows keep the
// source's order.
// @param offset is how many rows to skip; a negative value counts as zero.
// @param limit caps the rows returned. Empty means no cap; a negative
// value returns none.
// @param projection lists the fields to keep. Empty means every field.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct QueryShape {
  std::vector<QueryFilter> filters;
  std::vector<QuerySort> sort;
  int64_t offset = 0;
  std::optional<int64_t> limit;
  std::vector<std::string> projection;
};

// Purpose: Parse and check the filters of a request.
// @param request is the query or mutation request.
// @returns The filters, or the reason they are invalid.
// @throws Nothing.
std::variant<std::vector<QueryFilter>, std::string> ParseFilters(
    const flutter::EncodableMap& request);

// Purpose: Parse and check how a query request shapes its rows.
// @param request is the query request.
// @returns The shape, or the reason it is invalid.
// @throws Nothing.
std::variant<QueryShape, std::string> ParseQueryShape(const flutter::EncodableMap& request);

// Purpose: Keep the rows that pass every filter.
// @param rows are the rows to filter.
// @param filters are the parsed filters.
// @returns The rows that pass, in their original order.
// @throws Nothing.
std::vector<flutter::EncodableMap> ApplyFilters(std::vector<flutter::EncodableMap> rows,
                                                const std::vector<QueryFilter>& filters);

// Purpose: Filter, sort, page and project rows into a query result.
// @param rows are every row of the domain.
// @param shape is the parsed query shape.
// @returns The result map Dart reads: records, totalCount and, when more
// rows follow, nextOffset.
// @throws Nothing.
flutter::EncodableMap ShapeQueryResult(std::vector<flutter::EncodableMap> rows,
                                       const QueryShape& shape);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_QUERY_SHAPING_H_
