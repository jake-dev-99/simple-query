import 'models.dart';
import 'simple_query_platform.dart';

/// A test-only [SimpleQueryPlatform] implementation with safe defaults for
/// every method and per-method call counters.
///
/// Use as-is when a test only needs "some implementation is wired up", or
/// subclass to override individual methods. Every fake exported from this
/// library is intentionally simple; richer scenarios should live in each
/// test's own fixture rather than bloating this helper.
///
/// This type is exported from `simple_query_platform_interface/testing.dart`.
class FakeSimpleQueryPlatform extends SimpleQueryPlatform {
  FakeSimpleQueryPlatform({
    CapabilitySnapshot? capabilities,
    QueryResult? queryResult,
    MutationResult? mutationResult,
    BatchResult? batchResult,
    BinaryContentHandle? binaryContentHandle,
    Stream<ObserveEvent>? observeStream,
    Map<String, Object?>? extensionResult,
  })  : _capabilities = capabilities ?? _defaultCapabilities(),
        _queryResult = queryResult ?? const QueryResult(records: <QueryRecord>[]),
        _mutationResult =
            mutationResult ?? const MutationResult(affectedCount: 0),
        _batchResult = batchResult ?? const BatchResult(results: <MutationResult>[]),
        _binaryContentHandle = binaryContentHandle ??
            const BinaryContentHandle(handleId: 'h', localPath: '/tmp/h'),
        _observeStream = observeStream ?? const Stream<ObserveEvent>.empty(),
        _extensionResult = extensionResult;

  final CapabilitySnapshot _capabilities;
  final QueryResult _queryResult;
  final MutationResult _mutationResult;
  final BatchResult _batchResult;
  final BinaryContentHandle _binaryContentHandle;
  final Stream<ObserveEvent> _observeStream;
  final Map<String, Object?>? _extensionResult;

  int getCapabilitiesCalls = 0;
  int queryCalls = 0;
  int mutateCalls = 0;
  int batchCalls = 0;
  int observeCalls = 0;
  int openBinaryCalls = 0;
  int closeBinaryCalls = 0;
  int callExtensionCalls = 0;
  int disposeCalls = 0;

  final List<QueryRequest> queryRequests = <QueryRequest>[];
  final List<MutationRequest> mutationRequests = <MutationRequest>[];
  final List<BatchRequest> batchRequests = <BatchRequest>[];
  final List<ObserveRequest> observeRequests = <ObserveRequest>[];
  final List<BinaryRequest> binaryRequests = <BinaryRequest>[];

  @override
  Future<CapabilitySnapshot> getCapabilities() async {
    getCapabilitiesCalls += 1;
    return _capabilities;
  }

  @override
  Future<QueryResult> query(QueryRequest request) async {
    queryCalls += 1;
    queryRequests.add(request);
    return _queryResult;
  }

  @override
  Future<MutationResult> mutate(MutationRequest request) async {
    mutateCalls += 1;
    mutationRequests.add(request);
    return _mutationResult;
  }

  @override
  Future<BatchResult> batch(BatchRequest request) async {
    batchCalls += 1;
    batchRequests.add(request);
    return _batchResult;
  }

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) {
    observeCalls += 1;
    observeRequests.add(request);
    return _observeStream;
  }

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) async {
    openBinaryCalls += 1;
    binaryRequests.add(request);
    return _binaryContentHandle;
  }

  @override
  Future<void> closeBinary(String handleId) async {
    closeBinaryCalls += 1;
  }

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    callExtensionCalls += 1;
    return _extensionResult;
  }

  @override
  Future<void> dispose() async {
    disposeCalls += 1;
  }
}

CapabilitySnapshot _defaultCapabilities() {
  return CapabilitySnapshot(
    capabilities: <CapabilityDescriptor>[
      for (final domain in QueryDomain.values)
        CapabilityDescriptor(
          domain: domain,
          canRead: true,
          canWrite: true,
          canObserve: true,
          canStream: true,
        ),
    ],
  );
}
