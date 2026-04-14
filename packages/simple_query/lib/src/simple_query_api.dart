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

  Future<List<T>> queryTyped<T>(
    QueryRequest request,
    T Function(Map<String, Object?>) fromRecord,
  ) async {
    final response = await query(request);
    return response.records.map(fromRecord).toList(growable: false);
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
