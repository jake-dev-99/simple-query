library;

import 'dart:io';

import 'package:simple_query_macos/src/generated/native_query.g.dart' as native;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';

class SimpleQueryMacos extends DarwinQueryPlatform
    implements native.NativeQueryFlutterApi {
  SimpleQueryMacos() : this._withHostApi(native.NativeQueryHostApi());

  SimpleQueryMacos._withHostApi(native.NativeQueryHostApi hostApi) : super.create(
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
            isCurrentPlatform: () => Platform.isMacOS,
            prefix: 'macos',
            fallbackSource: 'simple_query_macos.polling',
            contactsExtensionMethods: _handleMacosContactsExtension,
            defaultRootPath: Platform.isMacOS
                ? _defaultRootPath()
                : null,
          ),
        );

  static void registerWith([Object? _]) {
    SimpleQueryPlatform.instance = SimpleQueryMacos();
  }

  @override
  void onObserveEvent(String observerId, Map<String?, Object?> event) {
    onObserveEventFromNative(observerId, event);
  }

  static Future<Map<String, Object?>?> _handleMacosContactsExtension({
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
      case 'listSources':
        ensureNoArgs(args, method: 'macos.contacts.listSources');
        return <String, Object?>{
          'sources': <Map<String, Object?>>[
            <String, Object?>{
              'id': 'default',
              'name': 'Default Contacts Source',
            },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupportedBuilder(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'macos.contacts method $method is not supported',
        );
    }
  }

  static String? _defaultRootPath() {
    final home = Platform.environment['HOME'];
    if (home != null) return '$home/Documents';
    return null;
  }
}
