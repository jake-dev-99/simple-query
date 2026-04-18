library;

import 'dart:io';

import 'package:simple_query_ios/src/generated/native_query.g.dart' as native;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';

class SimpleQueryIos extends DarwinQueryPlatform
    implements native.NativeQueryFlutterApi {
  SimpleQueryIos() : this._withHostApi(native.NativeQueryHostApi());

  SimpleQueryIos._withHostApi(native.NativeQueryHostApi hostApi)
      : super.create(
          bindings: NonAndroidHostBindings(
            setupFlutterApi: () {},
            getCapabilities: hostApi.getCapabilities,
            query: hostApi.query,
            mutate: hostApi.mutate,
            batch: hostApi.batch,
            observeStart: hostApi.observeStart,
            observeStop: hostApi.observeStop,
            openBinary: hostApi.openBinary,
            closeBinary: hostApi.closeBinary,
            callExtension: hostApi.callExtension,
          ),
          config: DarwinPlatformConfig(
            isCurrentPlatform: () => Platform.isIOS,
            prefix: 'ios',
            fallbackSource: 'simple_query_ios.polling',
            contactsExtensionMethods: _handleIosContactsExtension,
            defaultRootPath: _defaultRootPath(),
          ),
        );

  static void registerWith([Object? _]) {
    SimpleQueryPlatform.instance = SimpleQueryIos();
  }

  @override
  void onObserveEvent(String observerId, Map<String?, Object?> event) {
    onObserveEventFromNative(observerId, event);
  }

  static Future<Map<String, Object?>?> _handleIosContactsExtension({
    required String method,
    Map<String, Object?>? args,
    required void Function(Map<String, Object?>? args, {required String method})
        ensureNoArgs,
    required SimpleQueryError Function({
      required QueryOperation operation,
      QueryDomain? domain,
      required String reason,
    }) notSupportedBuilder,
    required SimpleQueryError Function(String message) invalidQueryBuilder,
  }) async {
    switch (method) {
      case 'listContainers':
        ensureNoArgs(args, method: 'ios.contacts.listContainers');
        return <String, Object?>{
          'containers': <Map<String, Object?>>[
            <String, Object?>{
              'id': 'default',
              'name': 'Default Contacts Container',
            },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      case 'listUnifiedSources':
        ensureNoArgs(args, method: 'ios.contacts.listUnifiedSources');
        return const <String, Object?>{
          'sources': <Map<String, Object?>>[
            <String, Object?>{
              'id': 'default',
              'name': 'Unified Contacts',
            },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      case 'requestScopedAccess':
        final scope = args?['scope'];
        if (scope != null && scope is! String) {
          throw invalidQueryBuilder(
            'simple_query: ios.contacts.requestScopedAccess expects scope as String',
          );
        }
        return const <String, Object?>{
          'granted': false,
          'status': 'unavailable',
          'reason': 'Native scoped contacts permission flow is not wired yet',
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupportedBuilder(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'ios.contacts method $method is not supported',
        );
    }
  }

  /// Returns the app's documents directory on iOS as a sensible default for
  /// filesystem fallback queries.
  static String? _defaultRootPath() {
    if (!Platform.isIOS) return null;
    // On iOS, the app sandbox's documents directory is the standard location.
    // The environment variable is not available, so we use a known iOS path.
    final home = Platform.environment['HOME'];
    if (home != null) return '$home/Documents';
    return null;
  }
}
