import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

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

  /// Query an arbitrary content provider by URI, bypassing the canonical
  /// schema.
  ///
  /// Use this for first-party providers (`content://com.biz.app/...`) or
  /// Android system providers the canonical [QueryDomain]s do not cover.
  /// Records come back as raw `Map<String, Object?>` with native column
  /// names exactly as the provider returned them — no canonical field
  /// translation, no contract validation, no typed record wrapping.
  ///
  /// Permission resolution still runs (see [simple_permissions_native] wiring
  /// on Android), so the authority embedded in [contentUri] must map to a
  /// granted permission.
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

  Future<BinaryContentHandle> openBinary(BinaryRequest request) {
    return SimpleQueryPlatform.instance.openBinary(request);
  }

  Future<void> closeBinary(String handleId) {
    return SimpleQueryPlatform.instance.closeBinary(handleId);
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
