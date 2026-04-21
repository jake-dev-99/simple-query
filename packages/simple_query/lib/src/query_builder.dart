import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'simple_query_api.dart';

/// A fluent builder for constructing and executing a [QueryRequest].
///
/// Use this instead of constructing [QueryRequest] directly when you want
/// a readable, chainable API. Recommended chain order is loosely:
///
/// 1. `entityType(...)` (optional, for sub-types like contact accounts)
/// 2. `where(...)` (zero or more)
/// 3. `select([...])` (optional projection)
/// 4. `orderBy(...)` (zero or more)
/// 5. `page(limit: ...)` or `pageOffset(...)` or `pageCursor(...)`
/// 6. `platformData({...})` (optional, for platformSpecific URIs etc.)
/// 7. `execute()` / `executeTyped(...)` / `build()`
///
/// All chain methods return `this`. Order doesn't actually affect the
/// built request, but the suggested order reads naturally.
///
/// Example:
/// ```dart
/// final calls = await SimpleQuery.instance
///     .queryBuilder(QueryDomain.calls)
///     .where('callType', QueryFilterOperator.equals, '2')
///     .orderBy('timestamp', direction: QuerySortDirection.descending)
///     .page(limit: 50)
///     .executeTyped(CallRecord.fromRecord);
/// ```
///
/// On `build()` (and therefore `execute()` / `executeTyped()`), every
/// canonical field used in `where(...)` / `orderBy(...)` / `select(...)`
/// is validated against [QueryFieldCatalog]. Unknown canonical fields
/// throw `SimpleQueryError(invalidQuery, details: {field, allowed})`
/// before any platform call. Skip this validation only by using
/// [QueryDomain.platformSpecific] (see [SimpleQuery.queryRaw]).
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

  /// Generic pagination. Caller is responsible for picking offset XOR
  /// cursor (the underlying [QueryPage] asserts mutual exclusion). Prefer
  /// [pageOffset] or [pageCursor] for clarity.
  QueryBuilder page({int? limit, int? offset, String? cursor}) {
    _page = QueryPage(limit: limit, offset: offset, cursor: cursor);
    return this;
  }

  /// Offset-based pagination convenience.
  QueryBuilder pageOffset({int? limit, required int offset}) {
    _page = QueryPage.offset(limit: limit, offset: offset);
    return this;
  }

  /// Cursor-based pagination convenience.
  QueryBuilder pageCursor({int? limit, required String cursor}) {
    _page = QueryPage.cursor(limit: limit, cursor: cursor);
    return this;
  }

  QueryBuilder platformData(Map<String, Object?> data) {
    _platformData = data;
    return this;
  }

  /// Builds the final [QueryRequest] and validates canonical-field usage
  /// against [QueryFieldCatalog]. Throws `SimpleQueryError(invalidQuery)`
  /// on the first unknown field encountered (no-op for
  /// [QueryDomain.platformSpecific]).
  QueryRequest build() {
    final request = QueryRequest(
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
    return RuntimeContractValidation.validateQueryRequest(request);
  }

  Future<QueryResult> execute() {
    return SimpleQuery.instance.query(build());
  }

  Future<List<T>> executeTyped<T>(
    T Function(Map<String, Object?> record) fromRecord,
  ) {
    return SimpleQuery.instance.queryTyped(build(), fromRecord);
  }

  /// Convenience for [SimpleQuery.queryPaginated] with the built request.
  Stream<QueryResult> executePaginated() {
    return SimpleQuery.instance.queryPaginated(build());
  }
}
