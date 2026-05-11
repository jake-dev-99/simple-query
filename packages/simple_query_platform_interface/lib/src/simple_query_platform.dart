import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'exceptions.dart';
import 'models.dart';
import 'runtime_validation.dart';

/// The federated contract every simple_query platform implementation
/// satisfies.
///
/// App code never talks to this class directly — use the facade
/// `SimpleQuery.instance` in `package:simple_query/simple_query.dart`.
/// Plugin authors and test code use [SimpleQueryPlatform.instance] to
/// install an implementation.
///
/// Each `simple_query_<platform>` package registers its subclass at
/// plugin-load time via `SimpleQueryPlatform.instance = ...`. Until a
/// subclass registers, [instance] returns an internal "unsupported"
/// implementation that throws [SimpleQueryError] with
/// `code: notSupported` for every operation.
abstract class SimpleQueryPlatform extends PlatformInterface {
  SimpleQueryPlatform() : super(token: _token);

  static final Object _token = Object();
  static SimpleQueryPlatform _instance = _UnsupportedSimpleQueryPlatform();

  /// The currently-registered platform implementation. Returns the
  /// internal unsupported stub until a concrete platform package calls
  /// the setter.
  static SimpleQueryPlatform get instance => _instance;

  /// Installs [instance] as the active platform implementation.
  ///
  /// Swapping the instance is **not thread-safe**: do not replace it
  /// while an in-flight operation is still awaiting a result. Tests
  /// typically install a fake in `setUp` and restore the original in
  /// `tearDown`; production plugin registrants install once during
  /// `registerWith` and never replace.
  ///
  /// The setter verifies [instance] was created with the private
  /// platform-interface token, preventing accidental third-party
  /// implementations that bypass the federated contract.
  static set instance(SimpleQueryPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// The opaque token used by [PlatformInterface.verifyToken]. Exposed
  /// only so platform packages can reference it in their own `super`
  /// calls; consumers should ignore it.
  static Object get token => _token;

  /// Reports, for every [QueryDomain], which operations
  /// (read / write / observe / stream) are usable on this device right
  /// now. Check this before issuing any request — the unsupported
  /// default reports everything as `false`.
  Future<CapabilitySnapshot> getCapabilities();

  /// Runs [request] against the platform backend. Throws
  /// [SimpleQueryError] on failure; the `code` names the reason
  /// (`notSupported`, `permissionDenied`, `invalidQuery`, `unavailable`,
  /// `transientFailure`).
  Future<QueryResult> query(QueryRequest request);

  /// Executes a single insert / update / delete. Throws
  /// [SimpleQueryError] on failure.
  Future<MutationResult> mutate(MutationRequest request);

  /// Executes a batch of mutations with `sequentialBestEffort`
  /// semantics: a failed operation records its error in its own
  /// [MutationResult.metadata] `error` key and does not abort the batch.
  Future<BatchResult> batch(BatchRequest request);

  /// Watches [ObserveRequest.domain] for changes. Emits
  /// [ObserveEvent]s until the subscription is cancelled. Unsupported
  /// platforms emit a single [SimpleQueryError] with
  /// `code: notSupported`.
  Stream<ObserveEvent> observe(ObserveRequest request);

  /// Opens binary content (e.g. a photo body) and returns a handle.
  /// Pair with [closeBinary]. Consumers should prefer the facade's
  /// `openBinaryContent` / `withBinaryContent` helpers, which wrap the
  /// raw handle with a self-closing lifecycle.
  Future<BinaryContentHandle> openBinary(BinaryRequest request);

  /// Releases a handle previously returned by [openBinary]. Safe to
  /// call more than once.
  Future<void> closeBinary(String handleId);

  /// Calls a platform-specific extension method. The available
  /// namespaces and methods are documented per platform in
  /// `docs/extensions/<platform>.md`. Returns `null` when the extension
  /// has no structured response.
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  });

  /// Releases platform resources (observers, open binary handles, etc.).
  /// Typically called once at app shutdown. Idempotent.
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
