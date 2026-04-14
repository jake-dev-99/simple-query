import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'exceptions.dart';
import 'models.dart';
import 'runtime_validation.dart';

abstract class SimpleQueryPlatform extends PlatformInterface {
  SimpleQueryPlatform() : super(token: _token);

  static final Object _token = Object();
  static SimpleQueryPlatform _instance = _UnsupportedSimpleQueryPlatform();

  static SimpleQueryPlatform get instance => _instance;

  static set instance(SimpleQueryPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  static Object get token => _token;

  Future<CapabilitySnapshot> getCapabilities();
  Future<QueryResult> query(QueryRequest request);
  Future<MutationResult> mutate(MutationRequest request);
  Future<BatchResult> batch(BatchRequest request);
  Stream<ObserveEvent> observe(ObserveRequest request);
  Future<BinaryContentHandle> openBinary(BinaryRequest request);
  Future<void> closeBinary(String handleId);
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  });
  Future<void> dispose();
}

class _UnsupportedSimpleQueryPlatform extends SimpleQueryPlatform {
  Never _unsupported({
    required QueryOperation operation,
    QueryDomain? domain,
  }) {
    throw SimpleQueryError(
      code: SimpleQueryErrorCode.notSupported,
      message:
          'simple_query: ${operation.name} is not supported on this platform',
      operation: operation,
      domain: domain,
    );
  }

  @override
  Future<BatchResult> batch(BatchRequest request) async =>
      _unsupported(operation: QueryOperation.write);

  @override
  Future<void> closeBinary(String handleId) async =>
      _unsupported(operation: QueryOperation.stream);

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async =>
      _unsupported(operation: QueryOperation.read);

  @override
  Future<void> dispose() async {}

  @override
  Future<CapabilitySnapshot> getCapabilities() async {
    return RuntimeContractValidation.validateCapabilitySnapshot(
      CapabilitySnapshot(
        capabilities: QueryDomain.values
            .map(
              (domain) => CapabilityDescriptor(
                domain: domain,
                canRead: false,
                canWrite: false,
                canObserve: false,
                canStream: false,
                reason: 'simple_query: platform implementation unavailable',
              ),
            )
            .toList(growable: false),
      ),
    );
  }

  @override
  Future<MutationResult> mutate(MutationRequest request) async =>
      _unsupported(operation: QueryOperation.write, domain: request.domain);

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) => Stream.error(
        SimpleQueryError(
          code: SimpleQueryErrorCode.notSupported,
          message: 'simple_query: observe is not supported on this platform',
          operation: QueryOperation.observe,
          domain: request.domain,
        ),
      );

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) async =>
      _unsupported(operation: QueryOperation.stream, domain: request.domain);

  @override
  Future<QueryResult> query(QueryRequest request) async =>
      _unsupported(operation: QueryOperation.read, domain: request.domain);
}
