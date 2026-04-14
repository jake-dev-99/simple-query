import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'filesystem_fallback.dart';
import 'non_android_native_bridge.dart';

typedef LocalExtensionHandler = Future<Map<String, Object?>?> Function({
  required String namespace,
  required String method,
  Map<String, Object?>? args,
});

class NonAndroidHostBindings {
  const NonAndroidHostBindings({
    required this.setupFlutterApi,
    required this.getCapabilities,
    required this.query,
    required this.mutate,
    required this.batch,
    required this.observeStart,
    required this.observeStop,
    required this.openBinary,
    required this.closeBinary,
    required this.callExtension,
  });

  final void Function() setupFlutterApi;
  final Future<NativePayload> Function() getCapabilities;
  final Future<NativePayload> Function(NativePayload request) query;
  final Future<NativePayload> Function(NativePayload request) mutate;
  final Future<NativePayload> Function(NativePayload request) batch;
  final Future<String> Function(NativePayload request) observeStart;
  final Future<void> Function(String observerId) observeStop;
  final Future<NativePayload> Function(NativePayload request) openBinary;
  final Future<void> Function(String handleId) closeBinary;
  final Future<NativeNullablePayload> Function(
    String namespace,
    String method,
    Map<String, Object?>? args,
  ) callExtension;
}

abstract class BaseNonAndroidSimpleQueryPlatform extends SimpleQueryPlatform {
  BaseNonAndroidSimpleQueryPlatform();

  late final LocalFileSystemFallback _localFallback;
  late final NonAndroidNativeBridge _nativeBridge;

  void initializeBase({
    required bool Function() isCurrentPlatform,
    required NonAndroidHostBindings bindings,
    required String fallbackSource,
    required String Function(QueryDomain domain) unsupportedReasonFor,
    required bool Function(QueryDomain domain) nativeDomainSupported,
    String? defaultRootPath,
  }) {
    _localFallback = LocalFileSystemFallback(
      source: fallbackSource,
      unsupportedReasonFor: unsupportedReasonFor,
      defaultRootPath: defaultRootPath,
    );
    _nativeBridge = NonAndroidNativeBridge(
      isCurrentPlatform: isCurrentPlatform,
      setupFlutterApi: bindings.setupFlutterApi,
      getCapabilities: bindings.getCapabilities,
      query: bindings.query,
      mutate: bindings.mutate,
      batch: bindings.batch,
      observeStart: bindings.observeStart,
      observeStop: bindings.observeStop,
      openBinary: bindings.openBinary,
      closeBinary: bindings.closeBinary,
      callExtension: bindings.callExtension,
      nativeDomainSupported: nativeDomainSupported,
    );
    _nativeBridge.ensureFlutterApiSetup();
  }

  CapabilitySnapshot get defaultCapabilities;

  LocalFileSystemFallback get localFallback => _localFallback;

  NonAndroidNativeBridge get nativeBridge => _nativeBridge;

  SimpleQueryError invalidQuery(String message) {
    return SimpleQueryError(
      code: SimpleQueryErrorCode.invalidQuery,
      message: message,
    );
  }

  SimpleQueryError notSupported({
    required QueryOperation operation,
    QueryDomain? domain,
    required String reason,
  }) {
    return SimpleQueryError(
      code: SimpleQueryErrorCode.notSupported,
      message: 'simple_query: ${operation.name} is not supported - $reason',
      domain: domain,
      operation: operation,
      details: <String, Object?>{'reason': reason},
    );
  }

  Future<Map<String, Object?>?> handleLocalExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  });

  @override
  Future<CapabilitySnapshot> getCapabilities() async {
    final nativeSnapshot = await _nativeBridge.getCapabilitiesOrNull();
    return nativeSnapshot ?? defaultCapabilities;
  }

  @override
  Future<QueryResult> query(QueryRequest request) async {
    final nativeResult = await _nativeBridge.queryOrNull(request);
    return nativeResult ?? _localFallback.query(request);
  }

  @override
  Future<MutationResult> mutate(MutationRequest request) async {
    final nativeResult = await _nativeBridge.mutateOrNull(request);
    return nativeResult ?? _localFallback.mutate(request);
  }

  @override
  Future<BatchResult> batch(BatchRequest request) async {
    final nativeResult = await _nativeBridge.batchOrNull(request);
    return nativeResult ?? _localFallback.batch(request);
  }

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) {
    return _nativeBridge.observeOrFallback(
      request,
      () => _localFallback.observe(request),
    );
  }

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) async {
    final nativeResult = await _nativeBridge.openBinaryOrNull(request);
    return nativeResult ?? _localFallback.openBinary(request);
  }

  @override
  Future<void> closeBinary(String handleId) async {
    final closed = await _nativeBridge.closeBinaryOrFalse(handleId);
    if (closed) return;
    await _localFallback.closeBinary(handleId);
  }

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    final nativeResult = await _nativeBridge.callExtensionOrNull(
      namespace: namespace,
      method: method,
      args: args,
    );
    if (nativeResult != null) {
      return nativeResult;
    }
    return handleLocalExtension(
      namespace: namespace,
      method: method,
      args: args,
    );
  }

  @override
  Future<void> dispose() async {
    await _nativeBridge.dispose();
    _localFallback.dispose();
  }

  void handleObserveEvent(String observerId, Map<String?, Object?> event) {
    _nativeBridge.onObserveEvent(observerId, event);
  }
}
