// Native tests for query shaping: each filter operator Dart sends, the
// rejection of malformed filters, sort order across value kinds, paging and
// projection.

#include "query_shaping.h"

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace simple_query_windows {
namespace test {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Describe one filter of a query request.
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
// @param field is the extra field's name.
// @param value is the extra field's value.
// @returns The row.
// @throws Nothing.
EncodableMap Row(const std::string& id, const std::string& field, EncodableValue value) {
  return {{EncodableValue("id"), EncodableValue(id)}, {EncodableValue(field), std::move(value)}};
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
  std::vector<EncodableMap> rows{Row("one", "size", EncodableValue(int64_t{1})),
                                 Row("two", "size", EncodableValue(int64_t{2})),
                                 Row("three", "size", EncodableValue(int64_t{3}))};
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
      Row("report", "name", EncodableValue("Quarterly Report.txt")),
      Row("photo", "name", EncodableValue("beach.jpg")),
      Row("tagged", "name",
          EncodableValue(EncodableList{EncodableValue("work"), EncodableValue("report")}))};

  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "name",
                                                 .op = "contains",
                                                 .value = EncodableValue("REPORT")}))),
            (std::vector<std::string>{"report"}));
  EXPECT_EQ(IdsOf(Shape(rows, RequestWithFilter({.field = "name",
                                                 .op = "contains",
                                                 .value = EncodableValue("report")}))),
            (std::vector<std::string>{"report", "tagged"}));
}

// Purpose: Large integers compare exactly, not through a rounded double.
TEST(QueryShapingTest, ComparesLargeIntegersExactly) {
  constexpr int64_t kBig = 9007199254740993;  // 2^53 + 1; a double rounds it.
  std::vector<EncodableMap> rows{Row("big", "size", EncodableValue(kBig)),
                                 Row("next", "size", EncodableValue(kBig - 1))};

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

// Purpose: Sorting puts absent values first, then numbers, then text, and
// descending reverses it.
TEST(QueryShapingTest, SortsMixedKindsInOneOrder) {
  std::vector<EncodableMap> rows{Row("text", "value", EncodableValue("b")),
                                 Row("number", "value", EncodableValue(int64_t{5})),
                                 {{EncodableValue("id"), EncodableValue("absent")}},
                                 Row("small", "value", EncodableValue(1.5))};
  const auto sorted = [&rows](const std::string& direction) {
    const EncodableMap sort_key{{EncodableValue("field"), EncodableValue("value")},
                                {EncodableValue("direction"), EncodableValue(direction)}};
    return IdsOf(Shape(rows, {{EncodableValue("sort"),
                               EncodableValue(EncodableList{EncodableValue(sort_key)})}}));
  };

  EXPECT_EQ(sorted("ascending"),
            (std::vector<std::string>{"absent", "small", "number", "text"}));
  EXPECT_EQ(sorted("descending"),
            (std::vector<std::string>{"text", "number", "small", "absent"}));
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
  std::vector<EncodableMap> rows{Row("a", "name", EncodableValue("notes.txt"))};
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

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
