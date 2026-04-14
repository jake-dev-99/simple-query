import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'simple_query_api.dart';

/// A fluent builder for constructing and executing a [QueryRequest].
///
/// Use this instead of creating [QueryRequest] directly when you want a
/// readable, chainable API (e.g. `QueryBuilder(domain).where(...).orderBy(...).execute()`).
class QueryBuilder {
  QueryBuilder(this._domain);

  final QueryDomain _domain;

  String? _entityType;
  final List<QueryFilterCondition> _filters = <QueryFilterCondition>[];
  List<String>? _projection;
  final List<QuerySort> _sort = <QuerySort>[];
  QueryPage? _page;
  Map<String, Object?>? _platformData;

  QueryBuilder entityType(String entityType) {
    _entityType = entityType;
    return this;
  }

  QueryBuilder where(
    String field,
    QueryFilterOperator operator,
    Object? value,
  ) {
    _filters.add(
      QueryFilterCondition(
        field: field,
        operator: operator,
        value: value,
      ),
    );
    return this;
  }

  QueryBuilder select(List<String> fields) {
    _projection = fields;
    return this;
  }

  QueryBuilder orderBy(
    String field, {
    QuerySortDirection direction = QuerySortDirection.ascending,
  }) {
    _sort.add(QuerySort(field: field, direction: direction));
    return this;
  }

  QueryBuilder page({int? limit, int? offset, String? cursor}) {
    _page = QueryPage(limit: limit, offset: offset, cursor: cursor);
    return this;
  }

  QueryBuilder platformData(Map<String, Object?> data) {
    _platformData = data;
    return this;
  }

  QueryRequest build() {
    return QueryRequest(
      domain: _domain,
      entityType: _entityType,
      filters: List<QueryFilterCondition>.unmodifiable(_filters),
      projection:
          _projection == null ? null : List<String>.unmodifiable(_projection!),
      sort: List<QuerySort>.unmodifiable(_sort),
      page: _page,
      platformData: _platformData == null
          ? null
          : Map<String, Object?>.unmodifiable(_platformData!),
    );
  }

  Future<QueryResult> execute() {
    return SimpleQuery.instance.query(build());
  }

  Future<List<T>> executeTyped<T>(
    T Function(Map<String, Object?> record) fromRecord,
  ) {
    return SimpleQuery.instance.queryTyped(build(), fromRecord);
  }
}
