import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';

void main() {
  test('observeOrFallback cancels fallback subscription on cancel', () async {
    final bridge = NonAndroidNativeBridge(
      isCurrentPlatform: () => true,
      setupFlutterApi: () {},
      getCapabilities: () async => <String?, Object?>{
        'capabilities': QueryDomain.values
            .map(
              (domain) => <String?, Object?>{
                'domain': domain.name,
                'canRead': true,
                'canWrite': true,
                'canObserve': true,
                'canStream': true,
              },
            )
            .toList(growable: false),
      },
      query: (_) async => <String?, Object?>{'records': const <Object?>[]},
      mutate: (_) async => <String?, Object?>{},
      batch: (_) async => <String?, Object?>{'results': const <Object?>[]},
      observeStart: (_) async => throw PlatformException(code: 'not-supported'),
      observeStop: (_) async {},
      openBinary: (_) async => <String?, Object?>{
        'handleId': 'h',
        'localPath': '/tmp/h',
      },
      closeBinary: (_) async {},
      callExtension: (_, __, ___) async => null,
      nativeDomainSupported: (_) => true,
    );

    var cancelCount = 0;
    late final StreamController<ObserveEvent> fallbackController;
    fallbackController = StreamController<ObserveEvent>.broadcast(
      onCancel: () {
        cancelCount += 1;
      },
    );

    final stream = bridge.observeOrFallback(
      const ObserveRequest(domain: QueryDomain.files),
      () => fallbackController.stream,
    );

    final subscription = stream.listen((_) {});
    await Future<void>.delayed(Duration.zero);
    await subscription.cancel();

    expect(cancelCount, 1);
    await fallbackController.close();
  });
}
