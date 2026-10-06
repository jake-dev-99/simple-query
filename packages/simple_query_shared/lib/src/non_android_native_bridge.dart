import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

typedef NativePayload = Map<String?, Object?>;
typedef NativeNullablePayload = Map<String?, Object?>?;

/// Purpose: Share native query transport and listener-owned observation cleanup.
/// @param observeStart creates native observer identifiers asynchronously.
/// @param observeStop releases those identifiers after cancellation or disposal.
/// @returns A bridge that translates platform replies into portable query models.
/// @throws Mapped query errors through operation futures or observation streams.
class NonAndroidNativeBridge {
  NonAndroidNativeBridge({
    required this.isCurrentPlatform,
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
    required this.nativeDomainSupported,
  });

  final bool Function() isCurrentPlatform;
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
  final bool Function(QueryDomain domain) nativeDomainSupported;

  bool _nativeUnavailable = false;
  bool _flutterApiSetup = false;

  // A unique listener-lifetime token prevents delayed startup replies from
  // registering after cancellation, disposal, or a newer listen cycle.
  final Map<StreamController<ObserveEvent>, Object> _observerLifetimes =
      <StreamController<ObserveEvent>, Object>{};
  // Native IDs and Dart controllers need both lookup directions for delivery
  // and cleanup. Fallback subscriptions are owned by the same listener lifetime.
  final Map<String, StreamController<ObserveEvent>> _nativeObservers =
      <String, StreamController<ObserveEvent>>{};
  final Map<StreamController<ObserveEvent>, String> _nativeObserverIds =
      <StreamController<ObserveEvent>, String>{};
  final Map<StreamController<ObserveEvent>, StreamSubscription<ObserveEvent>>
      _fallbackSubscriptions =
      <StreamController<ObserveEvent>, StreamSubscription<ObserveEvent>>{};

  void ensureFlutterApiSetup() {
    if (_flutterApiSetup || !isCurrentPlatform() || !_isBindingReady()) {
      return;
    }
    try {
      setupFlutterApi();
      _flutterApiSetup = true;
    } catch (_) {
      // Unit tests can run without a messenger initialized.
    }
  }

  void onObserveEvent(String observerId, NativePayload event) {
    // Removing the registration before asynchronous cleanup also rejects
    // events already queued by a cancelled native observer.
    final controller = _nativeObservers[observerId]; // ignore: close_sinks
    if (controller == null) return;
    controller.add(NativePayloadCodec.decodeObserveEvent(event));
  }

  Future<CapabilitySnapshot?> getCapabilitiesOrNull() async {
    if (!_shouldUseNativeBridge()) return null;
    try {
      return NativePayloadCodec.decodeCapabilitySnapshot(
        await getCapabilities(),
      );
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(error, operation: QueryOperation.read);
    }
  }

  Future<QueryResult?> queryOrNull(QueryRequest request) async {
    if (!_shouldUseNativeBridge() || !nativeDomainSupported(request.domain)) {
      return null;
    }
    try {
      return NativePayloadCodec.decodeQueryResult(
        await query(NativePayloadCodec.encodeQueryRequest(request)),
        domain: request.domain,
      );
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(
        error,
        domain: request.domain,
        operation: QueryOperation.read,
      );
    }
  }

  Future<MutationResult?> mutateOrNull(MutationRequest request) async {
    if (!_shouldUseNativeBridge() || !nativeDomainSupported(request.domain)) {
      return null;
    }
    try {
      return NativePayloadCodec.decodeMutationResult(
        await mutate(NativePayloadCodec.encodeMutationRequest(request)),
      );
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(
        error,
        domain: request.domain,
        operation: QueryOperation.write,
      );
    }
  }

  Future<BatchResult?> batchOrNull(BatchRequest request) async {
    if (!_shouldUseNativeBridge() ||
        !request.operations
            .every((item) => nativeDomainSupported(item.domain))) {
      return null;
    }
    try {
      return NativePayloadCodec.decodeBatchResult(
        await batch(NativePayloadCodec.encodeBatchRequest(request)),
      );
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(error, operation: QueryOperation.write);
    }
  }

  /// Purpose: Bind native or fallback observation to each listener lifetime.
  /// @param request selects the observed domain and native query.
  /// @param fallbackObserve supplies polling when native observation is unsupported.
  /// @returns A broadcast stream that releases each cancelled native observer.
  /// @throws Mapped native startup failures through the returned stream.
  Stream<ObserveEvent> observeOrFallback(
    ObserveRequest request,
    Stream<ObserveEvent> Function() fallbackObserve,
  ) {
    if (!_shouldUseNativeBridge() || !nativeDomainSupported(request.domain)) {
      return fallbackObserve();
    }

    late final StreamController<ObserveEvent> controller;
    controller = StreamController<ObserveEvent>.broadcast(
      onListen: () {
        final lifetime = Object();
        _observerLifetimes[controller] = lifetime;
        unawaited(_startObserver(
          controller: controller,
          lifetime: lifetime,
          request: request,
          fallbackObserve: fallbackObserve,
        ));
      },
      onCancel: () => unawaited(_cancelObserver(controller)),
    );
    return controller.stream;
  }

  /// Purpose: Reject replies belonging to an ended or superseded listen cycle.
  /// @param controller identifies the broadcast stream.
  /// @param lifetime identifies the startup that owns the reply.
  /// @returns Whether that startup still has listeners and current ownership.
  /// @throws Nothing.
  bool _isCurrentObserverLifetime({
    required StreamController<ObserveEvent> controller,
    required Object lifetime,
  }) =>
      identical(_observerLifetimes[controller], lifetime) &&
      controller.hasListener;

  /// Purpose: Register a successful start only while its listeners still own it.
  /// @param controller receives current observation events and mapped errors.
  /// @param lifetime identifies this asynchronous startup.
  /// @param request supplies the domain and query.
  /// @param fallbackObserve supplies unsupported-domain polling.
  /// @returns Completion after registration, fallback, or late-start cleanup.
  /// @throws Non-transport failures from injected native operations.
  Future<void> _startObserver({
    required StreamController<ObserveEvent> controller,
    required Object lifetime,
    required ObserveRequest request,
    required Stream<ObserveEvent> Function() fallbackObserve,
  }) async {
    try {
      ensureFlutterApiSetup();
      final observerId = await observeStart(
        NativePayloadCodec.encodeObserveRequest(request),
      );
      if (!_isCurrentObserverLifetime(
        controller: controller,
        lifetime: lifetime,
      )) {
        await _stopNativeObserver(observerId);
        return;
      }
      _nativeObservers[observerId] = controller;
      _nativeObserverIds[controller] = observerId;
    } on PlatformException catch (error) {
      if (!_isCurrentObserverLifetime(
        controller: controller,
        lifetime: lifetime,
      )) {
        return;
      }
      if (_shouldFallback(error)) {
        _startFallbackObserver(
          controller: controller,
          lifetime: lifetime,
          fallbackObserve: fallbackObserve,
        );
        return;
      }
      controller.addError(_mapPlatformException(
        error,
        domain: request.domain,
        operation: QueryOperation.observe,
      ));
    }
  }

  /// Purpose: Keep fallback completion from closing a newer listener lifetime.
  /// @param controller receives fallback events.
  /// @param lifetime identifies the current fallback ownership.
  /// @param fallbackObserve creates the fallback observation stream.
  /// @returns Nothing; cancellation ownership remains in the subscription map.
  /// @throws Failures constructing or listening to the fallback stream.
  void _startFallbackObserver({
    required StreamController<ObserveEvent> controller,
    required Object lifetime,
    required Stream<ObserveEvent> Function() fallbackObserve,
  }) {
    // The map owns cancellation; a local subscription cannot outlive its token.
    // ignore: cancel_subscriptions
    final subscription = fallbackObserve().listen(
      controller.add,
      onError: controller.addError,
      onDone: () {
        if (!identical(_observerLifetimes[controller], lifetime)) return;
        _observerLifetimes.remove(controller);
        _fallbackSubscriptions.remove(controller);
        unawaited(controller.close());
      },
    );
    _fallbackSubscriptions[controller] = subscription;
  }

  /// Purpose: Invalidate ownership before asynchronous last-listener cleanup.
  /// @param controller identifies the cancelled stream.
  /// @returns Completion after releasing its captured native or fallback owner.
  /// @throws Failures from a custom Flutter error handler.
  Future<void> _cancelObserver(
      StreamController<ObserveEvent> controller) async {
    if (controller.hasListener) return;
    _observerLifetimes.remove(controller);
    final subscription = _fallbackSubscriptions.remove(controller);
    final observerId = _nativeObserverIds.remove(controller);
    if (observerId != null) _nativeObservers.remove(observerId);
    if (subscription != null) await _cancelFallbackObserver(subscription);
    if (observerId != null) await _stopNativeObserver(observerId);
  }

  /// Purpose: Release a native observer without hiding transport cleanup errors.
  /// @param observerId identifies the native resource to stop.
  /// @returns Completion after cleanup or a contextual failure report.
  /// @throws Failures from a custom Flutter error handler.
  Future<void> _stopNativeObserver(String observerId) async {
    try {
      await observeStop(observerId);
    } catch (error, stackTrace) {
      _reportObserverCleanupFailure(
        error: error,
        stackTrace: stackTrace,
        action: 'stopping a native observer',
      );
    }
  }

  /// Purpose: Continue releasing other observers when fallback cancellation fails.
  /// @param subscription owns the fallback polling resource.
  /// @returns Completion after cancellation or a contextual failure report.
  /// @throws Failures from a custom Flutter error handler.
  Future<void> _cancelFallbackObserver(
    StreamSubscription<ObserveEvent> subscription,
  ) async {
    try {
      await subscription.cancel();
    } catch (error, stackTrace) {
      _reportObserverCleanupFailure(
        error: error,
        stackTrace: stackTrace,
        action: 'cancelling fallback observation',
      );
    }
  }

  /// Purpose: Surface cleanup failures without leaking raw transport exceptions.
  /// @param error is the failure to map and report.
  /// @param stackTrace identifies the cleanup failure location.
  /// @param action identifies the failed operation without resource contents.
  /// @returns Nothing.
  /// @throws Failures from a custom Flutter error handler.
  void _reportObserverCleanupFailure({
    required Object error,
    required StackTrace stackTrace,
    required String action,
  }) {
    FlutterError.reportError(FlutterErrorDetails(
      exception: error is PlatformException
          ? _mapPlatformException(error, operation: QueryOperation.observe)
          : error,
      stack: stackTrace,
      library: 'simple_query',
      context: ErrorDescription('while $action'),
    ));
  }

  Future<BinaryContentHandle?> openBinaryOrNull(BinaryRequest request) async {
    if (!_shouldUseNativeBridge() || !nativeDomainSupported(request.domain)) {
      return null;
    }
    try {
      return NativePayloadCodec.decodeBinaryHandle(
        await openBinary(NativePayloadCodec.encodeBinaryRequest(request)),
      );
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(
        error,
        domain: request.domain,
        operation: QueryOperation.stream,
      );
    }
  }

  Future<bool> closeBinaryOrFalse(String handleId) async {
    if (!_shouldUseNativeBridge()) return false;
    try {
      await closeBinary(handleId);
      return true;
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return false;
      }
      throw _mapPlatformException(error, operation: QueryOperation.stream);
    }
  }

  Future<Map<String, Object?>?> callExtensionOrNull({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    if (!_shouldUseNativeBridge()) return null;
    try {
      final payload = await callExtension(namespace, method, args);
      return payload?.map((key, value) => MapEntry(key ?? '', value));
    } on PlatformException catch (error) {
      if (_shouldFallback(error)) {
        return null;
      }
      throw _mapPlatformException(error, operation: QueryOperation.read);
    }
  }

  /// Purpose: Invalidate pending startups before releasing captured resources.
  /// @param None.
  /// @returns Completion after cancelling existing fallback and native owners.
  /// @throws Failures from a custom Flutter error handler.
  Future<void> dispose() async {
    final subscriptions = _fallbackSubscriptions.values.toList(growable: false);
    final observerIds = _nativeObservers.keys.toList(growable: false);
    _observerLifetimes.clear();
    _fallbackSubscriptions.clear();
    _nativeObservers.clear();
    _nativeObserverIds.clear();
    for (final subscription in subscriptions) {
      await _cancelFallbackObserver(subscription);
    }
    for (final observerId in observerIds) {
      await _stopNativeObserver(observerId);
    }
  }

  bool _shouldUseNativeBridge() =>
      isCurrentPlatform() && !_nativeUnavailable && _isBindingReady();

  bool _isBindingReady() {
    try {
      ServicesBinding.instance;
      return true;
    } catch (_) {
      return false;
    }
  }

  bool _shouldFallback(PlatformException error) {
    if (_isChannelError(error)) {
      _nativeUnavailable = true;
      return true;
    }
    return _isNotSupportedError(error);
  }

  bool _isChannelError(PlatformException error) =>
      error.code == 'channel-error' || error.code == 'null-error';

  bool _isNotSupportedError(PlatformException error) =>
      error.code == 'not-supported';

  SimpleQueryError _mapPlatformException(
    PlatformException error, {
    QueryDomain? domain,
    QueryOperation? operation,
  }) {
    SimpleQueryErrorCode code;
    switch (error.code) {
      case 'invalid-query':
        code = SimpleQueryErrorCode.invalidQuery;
      case 'permission-denied':
        code = SimpleQueryErrorCode.permissionDenied;
      case 'unavailable':
        code = SimpleQueryErrorCode.unavailable;
      case 'transient':
        code = SimpleQueryErrorCode.transientFailure;
      case 'not-supported':
      default:
        code = SimpleQueryErrorCode.notSupported;
    }
    return SimpleQueryError(
      code: code,
      message: error.message ?? 'simple_query: platform error',
      domain: domain,
      operation: operation,
      details: <String, Object?>{
        'code': error.code,
        'details': error.details,
      },
    );
  }
}

abstract final class NativePayloadCodec {
  static NativePayload encodeQueryRequest(QueryRequest request) {
    return <String?, Object?>{
      'domain': request.domain.name,
      'entityType': request.entityType,
      'filters': request.filters
          .map((item) => <String?, Object?>{
                'field': item.field,
                'operator': item.operator.name,
                'value': item.value,
              })
          .toList(growable: false),
      'projection': request.projection,
      'sort': request.sort
          .map((item) => <String?, Object?>{
                'field': item.field,
                'direction': item.direction.name,
              })
          .toList(growable: false),
      'page': request.page == null
          ? null
          : <String?, Object?>{
              'limit': request.page!.limit,
              'offset': request.page!.offset,
              'cursor': request.page!.cursor,
            },
      'platformData': request.platformData,
    };
  }

  static NativePayload encodeMutationRequest(MutationRequest request) {
    return <String?, Object?>{
      'domain': request.domain.name,
      'type': request.type.name,
      'entityType': request.entityType,
      'values': request.values,
      'filters': request.filters
          .map((item) => <String?, Object?>{
                'field': item.field,
                'operator': item.operator.name,
                'value': item.value,
              })
          .toList(growable: false),
      'platformData': request.platformData,
    };
  }

  static NativePayload encodeBatchRequest(BatchRequest request) {
    return <String?, Object?>{
      'operations': request.operations
          .map((item) => encodeMutationRequest(item))
          .toList(growable: false),
      'platformData': request.platformData,
    };
  }

  static NativePayload encodeObserveRequest(ObserveRequest request) {
    return <String?, Object?>{
      'domain': request.domain.name,
      'entityType': request.entityType,
      'filters': request.filters
          .map((item) => <String?, Object?>{
                'field': item.field,
                'operator': item.operator.name,
                'value': item.value,
              })
          .toList(growable: false),
      'pollingIntervalMs': request.pollingInterval?.inMilliseconds,
      'platformData': request.platformData,
    };
  }

  static NativePayload encodeBinaryRequest(BinaryRequest request) {
    return <String?, Object?>{
      'domain': request.domain.name,
      'entityType': request.entityType,
      'recordId': request.recordId,
      'platformData': request.platformData,
    };
  }

  static CapabilitySnapshot decodeCapabilitySnapshot(NativePayload payload) {
    final rawCapabilities = payload['capabilities'];
    final capabilities =
        (rawCapabilities as List<Object?>? ?? const <Object?>[])
            .whereType<Map<Object?, Object?>>()
            .map(
              (item) => CapabilityDescriptor(
                domain: QueryDomain.values.byName(item['domain']!.toString()),
                canRead: item['canRead'] == true,
                canWrite: item['canWrite'] == true,
                canObserve: item['canObserve'] == true,
                canStream: item['canStream'] == true,
                reason: item['reason']?.toString(),
              ),
            )
            .toList(growable: false);
    final platformExtensions =
        (payload['platformExtensions'] as Map<Object?, Object?>? ??
                const <Object?, Object?>{})
            .map((key, value) => MapEntry(key?.toString() ?? '', value));
    return RuntimeContractValidation.validateCapabilitySnapshot(
      CapabilitySnapshot(
        capabilities: capabilities,
        platformExtensions: platformExtensions,
      ),
    );
  }

  static QueryResult decodeQueryResult(
    NativePayload payload, {
    required QueryDomain domain,
  }) {
    final rawRecords =
        payload['records'] as List<Object?>? ?? const <Object?>[];
    final records = rawRecords
        .whereType<Map<Object?, Object?>>()
        .map(
          (row) =>
              row.map((key, value) => MapEntry(key?.toString() ?? '', value)),
        )
        .toList(growable: false);
    return RuntimeContractValidation.validateQueryResult(
      domain: domain,
      result: QueryResult(
        records: records,
        totalCount: _asInt(payload['totalCount']),
        nextOffset: _asInt(payload['nextOffset']),
        nextCursor: payload['nextCursor']?.toString(),
        metadata: _mapOrNull(payload['metadata']),
      ),
    );
  }

  static MutationResult decodeMutationResult(NativePayload payload) {
    return MutationResult(
      affectedCount: _asInt(payload['affectedCount']),
      insertedId: payload['insertedId']?.toString(),
      metadata: _mapOrNull(payload['metadata']),
    );
  }

  static BatchResult decodeBatchResult(NativePayload payload) {
    final rawResults =
        payload['results'] as List<Object?>? ?? const <Object?>[];
    return BatchResult(
      results: rawResults
          .whereType<Map<Object?, Object?>>()
          .map(
            (item) => decodeMutationResult(
              item.map((key, value) => MapEntry(key?.toString() ?? '', value)),
            ),
          )
          .toList(growable: false),
    );
  }

  static BinaryContentHandle decodeBinaryHandle(NativePayload payload) {
    return BinaryContentHandle(
      handleId: payload['handleId']?.toString() ?? '',
      localPath: payload['localPath']?.toString() ?? '',
      mimeType: payload['mimeType']?.toString(),
      size: _asInt(payload['size']),
      metadata: _mapOrNull(payload['metadata']),
    );
  }

  static ObserveEvent decodeObserveEvent(NativePayload payload) {
    return ObserveEvent(
      domain: QueryDomain.values.byName(payload['domain']!.toString()),
      changeType:
          ObserveChangeType.values.byName(payload['changeType']!.toString()),
      timestamp: DateTime.parse(payload['timestamp']!.toString()).toUtc(),
      entityType: payload['entityType']?.toString(),
      ids: (payload['ids'] as List<Object?>? ?? const <Object?>[])
          .map((item) => item.toString())
          .toList(growable: false),
      source: payload['source']?.toString(),
      metadata: _mapOrNull(payload['metadata']),
    );
  }

  static int? _asInt(Object? value) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    if (value is String) return int.tryParse(value);
    return null;
  }

  static Map<String, Object?>? _mapOrNull(Object? value) {
    final map = value as Map<Object?, Object?>?;
    if (map == null) return null;
    return map.map((key, item) => MapEntry(key?.toString() ?? '', item));
  }
}
