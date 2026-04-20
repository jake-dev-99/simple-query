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
