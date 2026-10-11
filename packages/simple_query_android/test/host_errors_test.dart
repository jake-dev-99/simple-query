import 'package:flutter/services.dart' show PlatformException;
import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_android/src/host_errors.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart'
    as iface;

void main() {
  group('mapHostError (UNFY-300)', () {
    test('names the host code when the host sent no message or target', () {
      final error = mapHostError(PlatformException(code: 'null-error'));

      expect(error.code, iface.SimpleQueryErrorCode.transientFailure);
      expect(error.message, 'simple_query: host call failed (null-error)');
      expect(error.domain, isNull);
      expect(error.operation, isNull);
      expect(error.details, <String, Object?>{
        'code': 'null-error',
        'details': null,
      });
    });

    test('a URI the host names in its details wins over the call target', () {
      final error = mapHostError(
        PlatformException(
          code: HostErrorCodes.unavailable,
          message: 'simple_query: content provider returned no cursor',
          details: <Object?, Object?>{'contentUri': 'content://mms/part'},
        ),
        contentUri: 'content://mms',
      );

      expect(error.details?['contentUri'], 'content://mms/part');
      expect(
          error.message, 'simple_query: content provider returned no cursor');
    });

    test('ignores non-string native fields rather than guessing them', () {
      final error = mapHostError(
        PlatformException(
          code: HostErrorCodes.invalidQuery,
          details: <Object?, Object?>{'contentUri': 7, 'nativeType': 9},
        ),
        contentUri: 'content://sms',
      );

      expect(error.code, iface.SimpleQueryErrorCode.invalidQuery);
      expect(error.details?['contentUri'], 'content://sms');
      expect(error.details?.containsKey('nativeType'), isFalse);
    });
  });

  group('guardHostCall (UNFY-300)', () {
    test('returns the host result untouched', () async {
      expect(await guardHostCall(() async => 42), 42);
    });

    test('lets a non-host failure through unmapped', () async {
      final bug = StateError('decoder bug');

      await expectLater(
        guardHostCall<void>(() async => throw bug),
        throwsA(same(bug)),
      );
    });
  });
}
