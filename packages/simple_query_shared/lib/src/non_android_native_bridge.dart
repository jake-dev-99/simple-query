import 'dart:async';

import 'package:flutter/services.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

typedef NativePayload = Map<String?, Object?>;
typedef NativeNullablePayload = Map<String?, Object?>?;

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

  // Observer lifecycle uses three maps because the native layer identifies
  // observers by string IDs while the Dart side uses StreamControllers.
  //
  // _nativeObservers:       observerId (from native) → controller
  // _nativeObserverIds:     controller → observerId  (reverse lookup for cleanup)
  // _fallbackSubscriptions: controller → subscription (when native failed and
  //                         we fell back to polling)
  //
  // On listen:  register native observer → store in both ID maps
  //             OR if native fails → subscribe to fallback → store in _fallbackSubscriptions
  // On cancel:  look up which path was taken → clean up the right map
  // On dispose: cancel everything in all three maps
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
    // `controller` is a borrowed reference from `_nativeObservers`; the
    // owning broadcast StreamController is closed from
    // `observeOrFallback`'s `onCancel` when the last listener drops.
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

  Stream<ObserveEvent> observeOrFallback(
    ObserveRequest request,
    Stream<ObserveEvent> Function() fallbackObserve,
  ) {
    if (!_shouldUseNativeBridge() || !nativeDomainSupported(request.domain)) {
      return fallbackObserve();
    }

    late final StreamController<ObserveEvent> controller;
    controller = StreamController<ObserveEvent>.broadcast(
      onListen: () async {
        try {
          ensureFlutterApiSetup();
          final observerId = await observeStart(
            NativePayloadCodec.encodeObserveRequest(request),
          );
          _nativeObservers[observerId] = controller;
          _nativeObserverIds[controller] = observerId;
        } on PlatformException catch (error) {
          if (_shouldFallback(error)) {
            // The subscription is stored in
            // `_fallbackSubscriptions[controller]` and cancelled from
            // the outer `onCancel` below. The lint can't follow the
            // map-mediated escape.
            // ignore: cancel_subscriptions
            final subscription = fallbackObserve().listen(
              controller.add,
              onError: controller.addError,
              onDone: () {
                _fallbackSubscriptions.remove(controller);
                controller.close();
              },
            );
            _fallbackSubscriptions[controller] = subscription;
            return;
          }
          controller.addError(
            _mapPlatformException(
              error,
              domain: request.domain,
              operation: QueryOperation.observe,
            ),
          );
        }
      },
      onCancel: () async {
        final fallbackSubscription = _fallbackSubscriptions.remove(controller);
        if (fallbackSubscription != null) {
          await fallbackSubscription.cancel();
        }
        if (controller.hasListener) return;
        final observerId = _nativeObserverIds.remove(controller);
        if (observerId == null) return;
        _nativeObservers.remove(observerId);
        try {
          await observeStop(observerId);
        } catch (_) {}
      },
    );
    return controller.stream;
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

  Future<void> dispose() async {
    for (final subscription
        in _fallbackSubscriptions.values.toList(growable: false)) {
      await subscription.cancel();
    }
    _fallbackSubscriptions.clear();
    for (final observerId in _nativeObservers.keys.toList(growable: false)) {
      try {
        await observeStop(observerId);
      } catch (_) {}
    }
    _nativeObservers.clear();
    _nativeObserverIds.clear();
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
