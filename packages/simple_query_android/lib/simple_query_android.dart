library;

import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'src/simple_query_android_api.dart';

class SimpleQueryAndroid extends SimpleQueryPlatform {
  SimpleQueryAndroid();

  static void registerWith() {
    SimpleQueryPlatform.instance = SimpleQueryAndroid();
  }

  final _api = SimpleQueryAndroidApi();

  @override
  Future<BatchResult> batch(BatchRequest request) => _api.batch(request);

  @override
  Future<void> closeBinary(String handleId) => _api.closeBinary(handleId);

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) =>
      _api.callExtension(namespace: namespace, method: method, args: args);

  @override
  Future<void> dispose() => _api.dispose();

  @override
  Future<CapabilitySnapshot> getCapabilities() => _api.getCapabilities();

  @override
  Future<MutationResult> mutate(MutationRequest request) =>
      _api.mutate(request);

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) =>
      _api.observe(request);

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) =>
      _api.openBinary(request);

  @override
  Future<QueryResult> query(QueryRequest request) => _api.query(request);
}
