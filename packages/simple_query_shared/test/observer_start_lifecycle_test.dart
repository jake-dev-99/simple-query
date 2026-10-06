import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';

/// Purpose: Control native replies while exercising the real bridge.
/// @param start supplies native identifiers or transport failures.
/// @param stop records native cleanup requests.
/// @returns A bridge using inert fixtures for unrelated operations.
/// @throws Nothing directly; injected operations can complete with errors.
NonAndroidNativeBridge _bridge({
  required Future<String> Function(Map<String?, Object?>) start,
  required Future<void> Function(String) stop,
}) =>
    NonAndroidNativeBridge(
      isCurrentPlatform: () => true,
      setupFlutterApi: () {},
      getCapabilities: () async => <String?, Object?>{},
      query: (_) async => <String?, Object?>{},
      mutate: (_) async => <String?, Object?>{},
      batch: (_) async => <String?, Object?>{},
      observeStart: start,
      observeStop: stop,
      openBinary: (_) async => <String?, Object?>{},
      closeBinary: (_) async {},
      callExtension: (_, __, ___) async => null,
      nativeDomainSupported: (_) => true,
    );

/// Purpose: Catch cancellation and disposal while native startup is pending.
/// @param None.
/// @returns Nothing.
/// @throws Nothing directly; registered tests report assertion failures.
void _pendingStartTests() {
  for (final action in <String>['cancel', 'dispose']) {
    test('$action stops a late successful native start', () async {
      final reply = Completer<String>();
      final invoked = Completer<void>();
      final stopped = <String>[];
      final bridge = _bridge(
        start: (_) {
          invoked.complete();
          return reply.future;
        },
        stop: (id) async => stopped.add(id),
      );
      final subscription = bridge
          .observeOrFallback(
            const ObserveRequest(domain: QueryDomain.files),
            () => const Stream<ObserveEvent>.empty(),
          )
          .listen((_) {});
      addTearDown(subscription.cancel);
      await invoked.future;
      if (action == 'cancel') {
        await subscription.cancel();
      } else {
        await bridge.dispose();
      }
      reply.complete('late-observer');
      await pumpEventQueue();
      expect(stopped, <String>['late-observer']);
    });
  }
}

/// Purpose: Reject fallback startup after the listener lifetime ends.
/// @param None.
/// @returns Nothing.
/// @throws Nothing directly; registered tests report assertion failures.
void _pendingFailureTests() {
  for (final action in <String>['cancel', 'dispose']) {
    test('$action prevents fallback after delayed unsupported reply', () async {
      final reply = Completer<String>();
      final invoked = Completer<void>();
      final bridge = _bridge(
        start: (_) {
          invoked.complete();
          return reply.future;
        },
        stop: (_) async {},
      );
      var fallbackCalls = 0;
      final subscription = bridge.observeOrFallback(
        const ObserveRequest(domain: QueryDomain.files),
        () {
          fallbackCalls += 1;
          return const Stream<ObserveEvent>.empty();
        },
      ).listen((_) {});
      addTearDown(subscription.cancel);
      await invoked.future;
      if (action == 'cancel') {
        await subscription.cancel();
      } else {
        await bridge.dispose();
      }
      reply.completeError(PlatformException(code: 'not-supported'));
      await pumpEventQueue();
      expect(fallbackCalls, 0);
    });
  }
}

/// Purpose: Keep old startup replies separate from relistened ownership.
/// @param None.
/// @returns Nothing.
/// @throws Nothing directly; registered tests report assertion failures.
void _relistenTests() {
  for (final order in <String>['old-first', 'new-first']) {
    test('relisten keeps separate ownership with $order completion', () async {
      final replies = <Completer<String>>[Completer(), Completer()];
      final invoked = <Completer<void>>[Completer(), Completer()];
      final stopped = <String>[];
      var starts = 0;
      final bridge = _bridge(
        start: (_) {
          final index = starts++;
          invoked[index].complete();
          return replies[index].future;
        },
        stop: (id) async => stopped.add(id),
      );
      final stream = bridge.observeOrFallback(
        const ObserveRequest(domain: QueryDomain.files),
        () => const Stream<ObserveEvent>.empty(),
      );
      final first = stream.listen((_) {});
      await invoked[0].future;
      await first.cancel();
      final received = <ObserveEvent>[];
      final second = stream.listen(received.add);
      addTearDown(second.cancel);
      await invoked[1].future;
      final indices = order == 'old-first' ? <int>[0, 1] : <int>[1, 0];
      for (final index in indices) {
        replies[index].complete(index == 0 ? 'old-observer' : 'new-observer');
        await pumpEventQueue();
      }
      expect(stopped, <String>['old-observer']);
      final payload = <String?, Object?>{
        'domain': 'files',
        'changeType': 'unknown',
        'timestamp': '2026-10-06T00:00:00.000Z',
        'ids': <String>['changed-record'],
      };
      bridge.onObserveEvent('old-observer', payload);
      bridge.onObserveEvent('new-observer', payload);
      await pumpEventQueue();
      expect(received.single.ids, <String>['changed-record']);
      await second.cancel();
      expect(stopped, <String>['old-observer', 'new-observer']);
    });
  }
}

/// Purpose: Preserve last-listener cleanup and report failed native cleanup.
/// @param None.
/// @returns Nothing.
/// @throws Nothing directly; registered tests report assertion failures.
void _cleanupTests() {
  test('only the last listener stops an active native observer', () async {
    final stopped = <String>[];
    final bridge = _bridge(
      start: (_) async => 'shared-observer',
      stop: (id) async => stopped.add(id),
    );
    final stream = bridge.observeOrFallback(
      const ObserveRequest(domain: QueryDomain.files),
      () => const Stream<ObserveEvent>.empty(),
    );
    final first = stream.listen((_) {});
    final second = stream.listen((_) {});
    await pumpEventQueue();
    await first.cancel();
    expect(stopped, isEmpty);
    await second.cancel();
    expect(stopped, <String>['shared-observer']);
  });

  test('failed late observer cleanup reports a mapped error', () async {
    final reply = Completer<String>();
    final reports = <FlutterErrorDetails>[];
    final originalHandler = FlutterError.onError;
    FlutterError.onError = reports.add;
    addTearDown(() => FlutterError.onError = originalHandler);
    final bridge = _bridge(
      start: (_) => reply.future,
      stop: (_) async => throw PlatformException(code: 'unavailable'),
    );
    final subscription = bridge
        .observeOrFallback(
          const ObserveRequest(domain: QueryDomain.files),
          () => const Stream<ObserveEvent>.empty(),
        )
        .listen((_) {});
    await subscription.cancel();
    reply.complete('failed-cleanup');
    await pumpEventQueue();
    expect(reports.single.exception, isA<SimpleQueryError>());
    final error = reports.single.exception as SimpleQueryError;
    expect(error.code, SimpleQueryErrorCode.unavailable);
    expect(error.operation, QueryOperation.observe);
  });
}

/// Purpose: Initialize the actual native bridge path and register race tests.
/// @param None.
/// @returns Nothing.
/// @throws Nothing directly; registered tests report contract failures.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  _pendingStartTests();
  _pendingFailureTests();
  _relistenTests();
  _cleanupTests();
}
