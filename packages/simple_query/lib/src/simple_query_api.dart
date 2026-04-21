import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'binary_content.dart';
import 'query_builder.dart';

/// The main entry point for querying and modifying device data.
///
/// Access it via [SimpleQuery.instance]. Use this to read contacts, media,
/// files, calendar events, and more across Android and iOS.
class SimpleQuery {
  SimpleQuery._();

  static final SimpleQuery instance = SimpleQuery._();

  Future<CapabilitySnapshot> getCapabilities() {
    return SimpleQueryPlatform.instance.getCapabilities();
  }

  Future<QueryResult> query(QueryRequest request) {
    return SimpleQueryPlatform.instance.query(request);
  }

  /// Returns a fluent [QueryBuilder] scoped to [domain]. Sugar over
  /// `QueryBuilder(domain)` — useful for chaining off `SimpleQuery.instance`.
  ///
  /// ```dart
  /// final calls = await SimpleQuery.instance
  ///     .queryBuilder(QueryDomain.calls)
  ///     .where('callType', QueryFilterOperator.equals, '2')
  ///     .orderBy('timestamp', direction: QuerySortDirection.descending)
  ///     .page(limit: 50)
  ///     .executeTyped(CallRecord.fromRecord);
  /// ```
  QueryBuilder queryBuilder(QueryDomain domain) => QueryBuilder(domain);

  /// Walks every page of [request] to exhaustion, emitting one
  /// [QueryResult] per page.
  ///
  /// Picks cursor-based pagination when the platform returns a
  /// [QueryResult.nextCursor], otherwise falls back to offset-based via
  /// [QueryResult.nextOffset]. Stops as soon as both are null or the
  /// platform returns an empty page.
  ///
  /// Use cases:
  /// - "Drain every record into a list":
  ///   `await SimpleQuery.instance.queryPaginated(req).expand((r) => r.records).toList();`
  /// - Stream-to-UI without loading every page in memory.
  /// - Cancel mid-flight by simply cancelling the stream subscription —
  ///   no further pages are fetched.
  ///
  /// Errors from any page propagate as a stream error and stop iteration.
  /// The first page uses [request] verbatim; subsequent pages reuse
  /// [QueryPage.limit] from the original request.
  Stream<QueryResult> queryPaginated(QueryRequest request) async* {
    var current = request;
    while (true) {
      final result = await query(current);
      yield result;

      final nextCursor = result.nextCursor;
      final nextOffset = result.nextOffset;
      // Defensive: an empty page with a non-null pagination token is a
      // platform bug; treat as exhausted to avoid spinning.
      if (result.records.isEmpty ||
          (nextCursor == null && nextOffset == null)) {
        return;
      }

      final limit = current.page?.limit;
      current = current.copyWith(
        page: nextCursor != null
            ? QueryPage.cursor(limit: limit, cursor: nextCursor)
            : QueryPage.offset(limit: limit, offset: nextOffset!),
      );
    }
  }

  /// Same shape as [queryPaginated] but maps each record through
  /// [fromRecord] before yielding. The stream emits one batch (a
  /// `List<T>`) per page.
  ///
  /// Mapping failures are wrapped in a [SimpleQueryError] with
  /// `code: invalidQuery` and `details.recordIndex` identifying the
  /// offending row, just like [queryTyped].
  Stream<List<T>> queryPaginatedTyped<T>(
    QueryRequest request,
    T Function(Map<String, Object?> record) fromRecord,
  ) async* {
    await for (final page in queryPaginated(request)) {
      final mapped = <T>[];
      for (var index = 0; index < page.records.length; index += 1) {
        final record = page.records[index];
        try {
          mapped.add(fromRecord(record));
        } on SimpleQueryError {
          rethrow;
        } catch (error, stackTrace) {
          Error.throwWithStackTrace(
            SimpleQueryError(
              code: SimpleQueryErrorCode.invalidQuery,
              message:
                  'simple_query: queryPaginatedTyped mapping failed for record at index $index',
              domain: request.domain,
              operation: QueryOperation.read,
              details: <String, Object?>{
                'domain': request.domain.name,
                'recordIndex': index,
                'cause': error.toString(),
              },
            ),
            stackTrace,
          );
        }
      }
      yield List<T>.unmodifiable(mapped);
    }
  }

  /// Query an arbitrary content provider by URI, bypassing the canonical
  /// schema.
  ///
  /// Use this for first-party providers (`content://com.biz.app/...`) or
  /// Android system providers the canonical [QueryDomain]s do not cover.
  /// Records come back as raw `Map<String, Object?>` with native column
  /// names exactly as the provider returned them — no canonical field
  /// translation, no contract validation, no typed record wrapping.
  ///
  /// Permission resolution still runs (Android's inline catalog maps the
  /// authority embedded in [contentUri] to one or more raw OS permission
  /// strings; missing grants throw `SimpleQueryError(permissionDenied,
  /// details: {permissions: [...]})` and the caller requests the
  /// permission via their own mechanism, then retries).
  ///
  /// Currently supported on Android only; on other platforms this throws
  /// [SimpleQueryError] with `code: notSupported`.
  ///
  /// This is sugar over `query(QueryRequest(domain: platformSpecific,
  /// platformData: {'contentUri': contentUri}, ...))` — if you need finer
  /// control (e.g. passing additional `platformData` keys), construct the
  /// [QueryRequest] directly.
  Future<QueryResult> queryRaw({
    required String contentUri,
    List<QueryFilterCondition> filters = const <QueryFilterCondition>[],
    List<String>? projection,
    List<QuerySort> sort = const <QuerySort>[],
    QueryPage? page,
    Map<String, Object?>? platformData,
  }) {
    final mergedPlatformData = <String, Object?>{
      ...?platformData,
      'contentUri': contentUri,
    };
    return query(
      QueryRequest(
        domain: QueryDomain.platformSpecific,
        filters: filters,
        projection: projection,
        sort: sort,
        page: page,
        platformData: mergedPlatformData,
      ),
    );
  }

  /// Runs [request] and maps each returned record through [fromRecord].
  ///
  /// If [fromRecord] throws for any record, the exception is wrapped in a
  /// [SimpleQueryError] with `code: invalidQuery` and `details` naming the
  /// failing `recordIndex` so callers can identify which row caused the
  /// failure. The original error is preserved in `details['cause']`.
  Future<List<T>> queryTyped<T>(
    QueryRequest request,
    T Function(Map<String, Object?>) fromRecord,
  ) async {
    final response = await query(request);
    final results = <T>[];
    for (var index = 0; index < response.records.length; index += 1) {
      final record = response.records[index];
      try {
        results.add(fromRecord(record));
      } on SimpleQueryError {
        rethrow;
      } catch (error, stackTrace) {
        Error.throwWithStackTrace(
          SimpleQueryError(
            code: SimpleQueryErrorCode.invalidQuery,
            message:
                'simple_query: queryTyped mapping failed for record at index $index',
            domain: request.domain,
            operation: QueryOperation.read,
            details: <String, Object?>{
              'domain': request.domain.name,
              'recordIndex': index,
              'cause': error.toString(),
            },
          ),
          stackTrace,
        );
      }
    }
    return List<T>.unmodifiable(results);
  }

  Future<MutationResult> mutate(MutationRequest request) {
    return SimpleQueryPlatform.instance.mutate(request);
  }

  Future<BatchResult> batch(BatchRequest request) {
    return SimpleQueryPlatform.instance.batch(request);
  }

  Stream<ObserveEvent> observe(ObserveRequest request) {
    return SimpleQueryPlatform.instance.observe(request);
  }

  /// Opens binary content (e.g. a photo body) and returns the wire-format
  /// handle. Most callers should prefer [openBinaryContent] or
  /// [withBinaryContent] which wrap the handle with a safe `close()`
  /// instead of forcing the caller to track a bare `handleId`.
  Future<BinaryContentHandle> openBinary(BinaryRequest request) {
    return SimpleQueryPlatform.instance.openBinary(request);
  }

  /// Closes a binary handle by id. Pair with [openBinary]. Most callers
  /// should prefer [openBinaryContent] / [withBinaryContent] which manage
  /// the handle automatically.
  Future<void> closeBinary(String handleId) {
    return SimpleQueryPlatform.instance.closeBinary(handleId);
  }

  /// Opens binary content as a self-closing [BinaryContent]. Caller is
  /// responsible for calling `close()` on the returned object — or use
  /// [withBinaryContent] for a scoped helper that closes on return.
  Future<BinaryContent> openBinaryContent(BinaryRequest request) async {
    final handle = await openBinary(request);
    return BinaryContent.forTesting(handle: handle, facade: this);
  }

  /// Opens binary content for the duration of [body], guaranteeing it is
  /// closed when [body] returns or throws. Returns whatever [body] returns.
  ///
  /// ```dart
  /// final bytes = await SimpleQuery.instance.withBinaryContent(
  ///   request,
  ///   (content) => File(content.localPath).readAsBytes(),
  /// );
  /// ```
  Future<R> withBinaryContent<R>(
    BinaryRequest request,
    Future<R> Function(BinaryContent content) body,
  ) async {
    final content = await openBinaryContent(request);
    try {
      return await body(content);
    } finally {
      await content.close();
    }
  }

  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) {
    return SimpleQueryPlatform.instance.callExtension(
      namespace: namespace,
      method: method,
      args: args,
    );
  }

  Future<void> dispose() async {
    await SimpleQueryPlatform.instance.dispose();
  }
}
