// Native tests for query shaping: each filter operator Dart sends, the
// rejection of malformed filters, sort order across value kinds, paging and
// projection.

#include "query_shaping.h"

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <utility>
#include <vector>

#include "request_values.h"

namespace simple_query_windows {
namespace test {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Describe one filter of a query request.
// @param field is sent as the filter's "field".
// @param op is the operator's wire name, such as "equals".
// @param value is sent as the filter's "value".
// @returns Nothing; this is a data type.
// @throws Nothing.
struct FilterSpec {
  std::string field;
  std::string op;
  EncodableValue value;
};

// Purpose: Build a query request with one filter.
// @param filter is the filter.
// @returns The request map.
// @throws Nothing.
EncodableMap RequestWithFilter(const FilterSpec& filter) {
  const EncodableMap filter_map{{EncodableValue("field"), EncodableValue(filter.field)},
                                {EncodableValue("operator"), EncodableValue(filter.op)},
                                {EncodableValue("value"), filter.value}};
  return {{EncodableValue("filters"), EncodableValue(EncodableList{EncodableValue(filter_map)})}};
}

// Purpose: Build a row with an id and one more field.
// @param id is the row's id.
// @param field is the extra field's name and value.
// @returns The row.
// @throws Nothing.
EncodableMap Row(const std::string& id, std::pair<std::string, EncodableValue> field) {
  return {{EncodableValue("id"), EncodableValue(id)},
          {EncodableValue(std::move(field.first)), std::move(field.second)}};
}

// Purpose: Shape rows with a request that must parse.
// @param rows are the rows.
// @param request is the query request.
// @returns The result map.
// @throws std::bad_variant_access when the request does not parse.
EncodableMap Shape(std::vector<EncodableMap> rows, const EncodableMap& request) {
  return ShapeQueryResult(std::move(rows), std::get<QueryShape>(ParseQueryShape(request)));
}

// Purpose: List the ids of a result's records, in order.
// @param result is a ShapeQueryResult map.
// @returns The ids.
// @throws std::bad_variant_access when a record has no string id.
std::vector<std::string> IdsOf(const EncodableMap& result) {
  std::vector<std::string> ids;
  for (const auto& record : std::get<EncodableList>(result.at(EncodableValue("records")))) {
    ids.push_back(
        std::get<std::string>(std::get<EncodableMap>(record).at(EncodableValue("id"))));
  }
  return ids;
}

// Purpose: Filter three rows whose size is 1, 2 and 3.
// @param op is the operator.
// @param value is the value compared against.
// @returns The ids of the rows that pass.
// @throws std::bad_variant_access when the filter does not parse.
std::vector<std::string> FilterSizes(const std::string& op, EncodableValue value) {
  std::vector<EncodableMap> rows{Row("one", {"size", EncodableValue(int64_t{1})}),
                                 Row("two", {"size", EncodableValue(int64_t{2})}),
                                 Row("three", {"size", EncodableValue(int64_t{3})})};
  return IdsOf(Shape(std::move(rows),
                     RequestWithFilter({.field = "size", .op = op, .value = std::move(value)})));
}

// Purpose: Each comparison operator keeps the rows it names.
TEST(QueryShapingTest, AppliesEveryComparisonOperator) {
  const EncodableValue two(int32_t{2});

  EXPECT_EQ(FilterSizes("equals", two), (std::vector<std::string>{"two"}));
  EXPECT_EQ(FilterSizes("notEquals", two), (std::vector<std::string>{"one", "three"}));
  EXPECT_EQ(FilterSizes("greaterThan", two), (std::vector<std::string>{"three"}));
  EXPECT_EQ(FilterSizes("greaterThanOrEqual", two), (std::vector<std::string>{"two", "three"}));
  EXPECT_EQ(FilterSizes("lessThan", two), (std::vector<std::string>{"one"}));
  EXPECT_EQ(FilterSizes("lessThanOrEqual", two), (std::vector<std::string>{"one", "two"}));
}

// Purpose: inList keeps rows whose value is one of the listed values.
TEST(QueryShapingTest, KeepsRowsInTheList) {
  const EncodableValue sizes(EncodableList{EncodableValue(int64_t{1}), EncodableValue(int64_t{3})});

  EXPECT_EQ(FilterSizes("inList", sizes), (std::vector<std::string>{"one", "three"}));
}

// Purpose: contains matches text without regard to case, and a list by
// element.
TEST(QueryShapingTest, MatchesContainsInTextAndLists) {
  std::vector<EncodableMap> rows{
      Row("report", {"name", EncodableValue("Quarterly Report.txt")}),
      Row("photo", {"name", EncodableValue("beach.jpg")}),
      Row("tagged", {"name", EncodableValue(EncodableList{EncodableValue("work"), EncodableValue("report")})})};

  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "name",
                                                 .op = "contains",
                                                 .value = EncodableValue("REPORT")}))),
            (std::vector<std::string>{"report"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "name",
                                                 .op = "contains",
                                                 .value = EncodableValue("report")}))),
            (std::vector<std::string>{"report", "tagged"}));
}

// Purpose: Filters against null match a missing or null field, as Dart's ==
// does; a files query uses this to find directories, which have no size.
TEST(QueryShapingTest, MatchesNullAgainstMissingAndNullFields) {
  const std::vector<EncodableMap> rows{
      Row("folder", {"size", EncodableValue()}),
      EncodableMap{{EncodableValue("id"), EncodableValue("missing")}},
      Row("file", {"size", EncodableValue(int64_t{3})})};
  const auto filter_size = [&rows](const std::string& op, EncodableValue value) {
    return IdsOf(
        Shape(rows, RequestWithFilter({.field = "size", .op = op, .value = std::move(value)})));
  };

  EXPECT_EQ(filter_size("equals", EncodableValue()),
            (std::vector<std::string>{"folder", "missing"}));
  EXPECT_EQ(filter_size("notEquals", EncodableValue()), (std::vector<std::string>{"file"}));
  EXPECT_EQ(filter_size("equals", EncodableValue(int64_t{3})), (std::vector<std::string>{"file"}));
  EXPECT_EQ(filter_size("notEquals", EncodableValue(int64_t{3})),
            (std::vector<std::string>{"folder", "missing"}));
  EXPECT_EQ(filter_size("inList", EncodableValue(EncodableList{EncodableValue()})),
            (std::vector<std::string>{"folder", "missing"}));
  EXPECT_TRUE(filter_size("greaterThan", EncodableValue()).empty());
}

// Purpose: A parsed shape keeps filtering correctly after its request is
// destroyed, since queries run on the background worker after the request
// that carried them is gone.
TEST(QueryShapingTest, FiltersAfterItsRequestIsDestroyed) {
  auto request = std::make_unique<EncodableMap>(
      RequestWithFilter({.field = "name", .op = "equals", .value = EncodableValue("kept")}));
  QueryShape shape = std::get<QueryShape>(ParseQueryShape(*request));
  // Overwrite, then free, the request so a shape still pointing into it
  // would compare against different or freed values.
  (*request)[EncodableValue("filters")] = EncodableValue(EncodableList{});
  request.reset();
  std::vector<EncodableMap> rows{Row("kept", {"name", EncodableValue("kept")}),
                                 Row("dropped", {"name", EncodableValue("dropped")})};

  EXPECT_EQ(IdsOf(ShapeQueryResult(std::move(rows), shape)),
            (std::vector<std::string>{"kept"}));
}

// Purpose: Large integers compare exactly, not through a rounded double.
TEST(QueryShapingTest, ComparesLargeIntegersExactly) {
  constexpr int64_t kBig = 9007199254740993;  // 2^53 + 1; a double rounds it.
  std::vector<EncodableMap> rows{Row("big", {"size", EncodableValue(kBig)}),
                                 Row("next", {"size", EncodableValue(kBig - 1)})};

  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size",
                                                 .op = "equals",
                                                 .value = EncodableValue(kBig)}))),
            (std::vector<std::string>{"big"}));
}

// Purpose: An operator Dart does not define is rejected, not ignored.
TEST(QueryShapingTest, RejectsAnUnknownOperator) {
  const auto parsed = ParseQueryShape(
      RequestWithFilter({.field = "size", .op = "startsWith", .value = EncodableValue("a")}));

  ASSERT_TRUE(std::holds_alternative<std::string>(parsed));
  EXPECT_NE(std::get<std::string>(parsed).find("startsWith"), std::string::npos);
}

// Purpose: inList without a list is rejected, not treated as matching all.
TEST(QueryShapingTest, RejectsInListWithoutAList) {
  const auto parsed = ParseQueryShape(
      RequestWithFilter({.field = "size", .op = "inList", .value = EncodableValue(int64_t{1})}));

  EXPECT_TRUE(std::holds_alternative<std::string>(parsed));
}

// Purpose: A filter that is not a map, or lacks a field, is rejected.
TEST(QueryShapingTest, RejectsAMalformedFilter) {
  const EncodableMap not_a_map{
      {EncodableValue("filters"), EncodableValue(EncodableList{EncodableValue("size")})}};
  const EncodableMap no_field{
      {EncodableValue("filters"),
       EncodableValue(EncodableList{EncodableValue(EncodableMap{
           {EncodableValue("operator"), EncodableValue("equals")}})})}};

  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(not_a_map)));
  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(no_field)));
}

// Purpose: A present filters value must be a list, so malformed input
// cannot become an unrestricted query or mutation.
TEST(QueryShapingTest, RejectsFiltersThatAreNotAList) {
  const std::vector<EncodableValue> malformed{
      EncodableValue(EncodableMap{}), EncodableValue("filters"), EncodableValue(true),
      EncodableValue(int32_t{1}), EncodableValue(int64_t{1}), EncodableValue(1.5)};

  for (const auto& filters : malformed) {
    SCOPED_TRACE(filters.index());
    const EncodableMap request{{EncodableValue("filters"), filters}};
    EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(request)));
  }
}

// Purpose: Omitted, null and empty filters retain the unrestricted semantics
// of valid requests.
TEST(QueryShapingTest, AcceptsOmittedNullAndEmptyFilters) {
  const std::vector<EncodableMap> requests{
      {}, {{EncodableValue("filters"), EncodableValue()}},
      {{EncodableValue("filters"), EncodableValue(EncodableList{})}}};
  const std::vector<EncodableMap> rows{Row("kept", {"size", EncodableValue(int64_t{1})})};

  for (const auto& request : requests) {
    const auto parsed = ParseQueryShape(request);
    ASSERT_TRUE(std::holds_alternative<QueryShape>(parsed));
    EXPECT_EQ(IdsOf(ShapeQueryResult(rows, std::get<QueryShape>(parsed))),
              (std::vector<std::string>{"kept"}));
  }
}

// Purpose: A NaN filter value is rejected for every operator, because NaN
// has no order and would otherwise match every row.
TEST(QueryShapingTest, RejectsANanFilterValue) {
  const EncodableValue nan(std::numeric_limits<double>::quiet_NaN());
  const EncodableValue nan_in_list(EncodableList{EncodableValue(int64_t{1}), nan});

  for (const std::string op :
       {"equals", "notEquals", "greaterThan", "lessThanOrEqual", "contains"}) {
    EXPECT_TRUE(std::holds_alternative<std::string>(
        ParseQueryShape(RequestWithFilter({.field = "size", .op = op, .value = nan}))))
        << op;
  }
  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(
      RequestWithFilter({.field = "size", .op = "inList", .value = nan_in_list}))));
}

// Purpose: A NaN field equals nothing and is neither greater nor less than
// any number, as in Dart.
TEST(QueryShapingTest, NeverOrdersANanField) {
  const auto filter_nan_row = [](const std::string& op) {
    std::vector<EncodableMap> rows{
        Row("nan", {"size", EncodableValue(std::numeric_limits<double>::quiet_NaN())}),
        Row("two", {"size", EncodableValue(int64_t{2})})};
    return IdsOf(Shape(std::move(rows),
                       RequestWithFilter(
                           {.field = "size", .op = op, .value = EncodableValue(int64_t{2})})));
  };

  EXPECT_EQ(filter_nan_row("equals"), (std::vector<std::string>{"two"}));
  EXPECT_EQ(filter_nan_row("notEquals"), (std::vector<std::string>{"nan"}));
  EXPECT_EQ(filter_nan_row("greaterThanOrEqual"), (std::vector<std::string>{"two"}));
  EXPECT_EQ(filter_nan_row("lessThanOrEqual"), (std::vector<std::string>{"two"}));
}

// Purpose: Sorting puts absent values first, then numbers, then NaN, then
// text, and descending reverses it.
TEST(QueryShapingTest, SortsMixedKindsInOneOrder) {
  std::vector<EncodableMap> rows{
      Row("text", {"value", EncodableValue("b")}),
      Row("nan", {"value", EncodableValue(std::numeric_limits<double>::quiet_NaN())}),
      Row("number", {"value", EncodableValue(int64_t{5})}),
      {{EncodableValue("id"), EncodableValue("absent")}},
      Row("small", {"value", EncodableValue(1.5)})};
  const auto sorted = [&rows](const std::string& direction) {
    const EncodableMap sort_key{{EncodableValue("field"), EncodableValue("value")},
                                {EncodableValue("direction"), EncodableValue(direction)}};
    return IdsOf(Shape(rows, {{EncodableValue("sort"),
                               EncodableValue(EncodableList{EncodableValue(sort_key)})}}));
  };

  EXPECT_EQ(sorted("ascending"),
            (std::vector<std::string>{"absent", "small", "number", "nan", "text"}));
  EXPECT_EQ(sorted("descending"),
            (std::vector<std::string>{"text", "nan", "number", "small", "absent"}));
}

// Purpose: A sort direction other than ascending or descending is rejected.
TEST(QueryShapingTest, RejectsAnUnknownSortDirection) {
  const EncodableMap sort_key{{EncodableValue("field"), EncodableValue("value")},
                              {EncodableValue("direction"), EncodableValue("sideways")}};
  const EncodableMap request{
      {EncodableValue("sort"), EncodableValue(EncodableList{EncodableValue(sort_key)})}};

  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(request)));
}

// Purpose: A page reports the total before paging and where the next page
// starts.
TEST(QueryShapingTest, PagesAndReportsTheNextOffset) {
  std::vector<EncodableMap> rows;
  for (int index = 0; index < 5; ++index) {
    rows.push_back({{EncodableValue("id"), EncodableValue("row" + std::to_string(index))}});
  }
  const EncodableMap page{{EncodableValue("offset"), EncodableValue(int32_t{1})},
                          {EncodableValue("limit"), EncodableValue(int32_t{2})}};

  const EncodableMap result = Shape(rows, {{EncodableValue("page"), EncodableValue(page)}});

  EXPECT_EQ(IdsOf(result), (std::vector<std::string>{"row1", "row2"}));
  EXPECT_EQ(std::get<int64_t>(result.at(EncodableValue("totalCount"))), 5);
  EXPECT_EQ(std::get<int64_t>(result.at(EncodableValue("nextOffset"))), 3);
}

// Purpose: The last page has no next offset.
TEST(QueryShapingTest, OmitsTheNextOffsetOnTheLastPage) {
  std::vector<EncodableMap> rows{{{EncodableValue("id"), EncodableValue("only")}}};
  const EncodableMap page{{EncodableValue("limit"), EncodableValue(int32_t{10})}};

  const EncodableMap result = Shape(rows, {{EncodableValue("page"), EncodableValue(page)}});

  EXPECT_EQ(result.count(EncodableValue("nextOffset")), 0u);
}

// Purpose: A page number that is not a number is rejected.
TEST(QueryShapingTest, RejectsAPageNumberThatIsNotANumber) {
  const EncodableMap page{{EncodableValue("offset"), EncodableValue("ten")}};

  EXPECT_TRUE(std::holds_alternative<std::string>(
      ParseQueryShape({{EncodableValue("page"), EncodableValue(page)}})));
}

// Purpose: A projection keeps only its fields, with a missing one as null,
// and rejects a field name that is not text.
TEST(QueryShapingTest, ProjectsFieldsAndRejectsNonTextNames) {
  std::vector<EncodableMap> rows{Row("a", {"name", EncodableValue("notes.txt")})};
  const EncodableMap request{
      {EncodableValue("projection"),
       EncodableValue(EncodableList{EncodableValue("name"), EncodableValue("size")})}};

  const EncodableMap result = Shape(rows, request);
  const auto& record =
      std::get<EncodableMap>(std::get<EncodableList>(result.at(EncodableValue("records")))[0]);

  EXPECT_EQ(record.size(), 2u);
  EXPECT_EQ(std::get<std::string>(record.at(EncodableValue("name"))), "notes.txt");
  EXPECT_TRUE(record.at(EncodableValue("size")).IsNull());
  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape(
      {{EncodableValue("projection"), EncodableValue(EncodableList{EncodableValue(7)})}})));
}

// Purpose: Mixed integer/double comparisons keep integers above 2^53 exact.
TEST(QueryShapingTest, OrdersMixedNumbersWithoutRoundingIntegers) {
  const std::vector<EncodableMap> rows{
      Row("integer", {"size", EncodableValue(int64_t{9007199254740993})}),
      Row("double", {"size", EncodableValue(9007199254740992.0)}),
      Row("smaller", {"size", EncodableValue(int64_t{9007199254740992})})};
  const EncodableMap sort{{EncodableValue("field"), EncodableValue("size")}};
  const EncodableMap request{{EncodableValue("sort"), EncodableValue(EncodableList{EncodableValue(sort)})}};
  EXPECT_EQ(IdsOf(Shape(rows, request)), (std::vector<std::string>{"double", "smaller", "integer"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "equals", .value = EncodableValue(9007199254740992.0)}))),
            (std::vector<std::string>{"double", "smaller"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "greaterThan", .value = EncodableValue(9007199254740992.0)}))),
            (std::vector<std::string>{"integer"}));
}

// Purpose: Mixed numbers compare at int64 boundaries and across fractions.
TEST(QueryShapingTest, ComparesMixedNumbersAtBoundsAndFractions) {
  const std::vector<EncodableMap> rows{
      Row("max", {"size", EncodableValue(std::numeric_limits<int64_t>::max())}),
      Row("min", {"size", EncodableValue(std::numeric_limits<int64_t>::min())}),
      Row("negative", {"size", EncodableValue(int64_t{-1})}),
      Row("positive", {"size", EncodableValue(int64_t{1})})};
  EXPECT_TRUE(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "equals", .value = EncodableValue(0x1p63)}))).empty());
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "equals", .value = EncodableValue(-0x1p63)}))),
            (std::vector<std::string>{"min"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "lessThan", .value = EncodableValue(-1.5)}))),
            (std::vector<std::string>{"min"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "size", .op = "greaterThan", .value = EncodableValue(1.5)}))),
            (std::vector<std::string>{"max"}));
}

// Purpose: Equality and membership never coerce strings, numbers or bools.
TEST(QueryShapingTest, KeepsScalarKindsDistinctForEqualityAndMembership) {
  const std::vector<EncodableMap> rows{
      Row("text", {"value", EncodableValue("1")}),
      Row("integer", {"value", EncodableValue(int64_t{1})}),
      Row("double", {"value", EncodableValue(1.0)}),
      Row("bool", {"value", EncodableValue(true)}),
      Row("bool_text", {"value", EncodableValue("true")})};
  for (const std::string op : {"equals", "inList"}) {
    const EncodableValue expected = op == "inList"
        ? EncodableValue(EncodableList{EncodableValue("1")}) : EncodableValue("1");
    EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "value", .op = op, .value = expected}))),
              (std::vector<std::string>{"text"}));
  }
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "value", .op = "equals", .value = EncodableValue(true)}))),
            (std::vector<std::string>{"bool"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "value", .op = "inList", .value = EncodableValue(EncodableList{EncodableValue(1.0)})}))),
            (std::vector<std::string>{"integer", "double"}));
}

// Purpose: Every sort key breaks ties in request order before paging.
TEST(QueryShapingTest, AppliesEverySortKeyBeforePaging) {
  const std::vector<EncodableMap> rows{
      {{EncodableValue("id"), EncodableValue("a")}, {EncodableValue("group"), EncodableValue(1)}, {EncodableValue("rank"), EncodableValue(1)}},
      {{EncodableValue("id"), EncodableValue("b")}, {EncodableValue("group"), EncodableValue(1)}, {EncodableValue("rank"), EncodableValue(2)}},
      {{EncodableValue("id"), EncodableValue("c")}, {EncodableValue("group"), EncodableValue(0)}, {EncodableValue("rank"), EncodableValue(1)}}};
  const EncodableList sort{
      EncodableValue(EncodableMap{{EncodableValue("field"), EncodableValue("group")}}),
      EncodableValue(EncodableMap{{EncodableValue("field"), EncodableValue("rank")}, {EncodableValue("direction"), EncodableValue("descending")}})};
  const EncodableMap page{{EncodableValue("offset"), EncodableValue(1)}, {EncodableValue("limit"), EncodableValue(1)}};
  EXPECT_EQ(IdsOf(Shape(rows, {{EncodableValue("sort"), EncodableValue(sort)}, {EncodableValue("page"), EncodableValue(page)}})),
            (std::vector<std::string>{"b"}));
  EncodableList invalid = sort;
  invalid.push_back(EncodableValue(EncodableMap{{EncodableValue("field"), EncodableValue("id")}, {EncodableValue("direction"), EncodableValue("sideways")}}));
  EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape({{EncodableValue("sort"), EncodableValue(invalid)}})));
}

// Purpose: Page numbers reject fractions and doubles outside int64 safely.
TEST(QueryShapingTest, RejectsFractionalAndOutOfRangePages) {
  const std::vector<double> invalid{0.5, -0.5, 0x1p63, -0x1.0000000000001p63,
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity()};
  for (const std::string key : {"offset", "limit"}) {
    for (const double value : invalid) {
      SCOPED_TRACE(key + ": " + std::to_string(value));
      const EncodableMap page{{EncodableValue(key), EncodableValue(value)}};
      EXPECT_TRUE(std::holds_alternative<std::string>(ParseQueryShape({{EncodableValue("page"), EncodableValue(page)}})));
    }
  }
}

// Purpose: Empty pages with zero or negative limits cannot repeat a token.
TEST(QueryShapingTest, OmitsNonAdvancingPageTokens) {
  const std::vector<EncodableMap> rows{Row("a", {"size", EncodableValue(1)}), Row("b", {"size", EncodableValue(2)})};
  for (const int limit : {0, -1}) {
    const EncodableMap page{{EncodableValue("offset"), EncodableValue(1)}, {EncodableValue("limit"), EncodableValue(limit)}};
    const auto result = Shape(rows, {{EncodableValue("page"), EncodableValue(page)}});
    EXPECT_TRUE(IdsOf(result).empty());
    EXPECT_EQ(result.count(EncodableValue("nextOffset")), 0u);
  }
}

// Purpose: Integer conversion rejects unsafe doubles without a cast overflow.
TEST(QueryShapingTest, ConvertsOnlyFiniteDoublesInInt64Range) {
  const std::vector<double> invalid{0x1p63, -0x1.0000000000001p63,
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity()};
  for (const double number : invalid) {
    const EncodableValue value(number);
    EXPECT_FALSE(AsInt(&value).has_value());
  }
  const EncodableValue minimum(-0x1p63);
  const EncodableValue below_maximum(std::nextafter(0x1p63, 0.0));
  const EncodableValue fraction(-1.75);
  EXPECT_EQ(AsInt(&minimum), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(AsInt(&below_maximum), int64_t{9223372036854774784});
  EXPECT_EQ(AsInt(&fraction), int64_t{-1});
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
