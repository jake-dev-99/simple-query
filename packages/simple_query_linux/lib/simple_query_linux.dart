library;

import 'dart:io';

import 'package:simple_query_linux/src/generated/native_query.g.dart' as native;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';

class SimpleQueryLinux extends BaseNonAndroidSimpleQueryPlatform
    implements native.NativeQueryFlutterApi {
  SimpleQueryLinux() {
    initializeBase(
      isCurrentPlatform: () => Platform.isLinux,
      fallbackSource: 'simple_query_linux.polling',
      unsupportedReasonFor: _localUnsupportedReason,
      nativeDomainSupported: _nativeDomainSupported,
      defaultRootPath: Platform.isLinux ? Platform.environment['HOME'] : null,
      bindings: NonAndroidHostBindings(
        setupFlutterApi: () => native.NativeQueryFlutterApi.setUp(this),
        getCapabilities: _nativeHostApi.getCapabilities,
        query: _nativeHostApi.query,
        mutate: _nativeHostApi.mutate,
        batch: _nativeHostApi.batch,
        observeStart: _nativeHostApi.observeStart,
        observeStop: _nativeHostApi.observeStop,
        openBinary: _nativeHostApi.openBinary,
        closeBinary: _nativeHostApi.closeBinary,
        callExtension: _nativeHostApi.callExtension,
      ),
    );
  }

  static void registerWith([Object? _]) {
    SimpleQueryPlatform.instance = SimpleQueryLinux();
  }

  final native.NativeQueryHostApi _nativeHostApi = native.NativeQueryHostApi();
  @override
  Future<Map<String, Object?>?> handleLocalExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (namespace) {
      case 'linux.eds':
        return _handleEdsExtension(method: method, args: args);
      case 'linux.tracker':
        return _handleTrackerExtension(method: method, args: args);
      case 'linux.xdg':
        return _handleXdgExtension(method: method, args: args);
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason:
              'Extension namespace $namespace is not supported on Linux backend',
        );
    }
  }

  @override
  void onObserveEvent(String observerId, Map<String?, Object?> event) {
    handleObserveEvent(observerId, event);
  }

  @override
  CapabilitySnapshot get defaultCapabilities =>
      RuntimeContractValidation.validateCapabilitySnapshot(
        const CapabilitySnapshot(
          capabilities: <CapabilityDescriptor>[
            CapabilityDescriptor(
              domain: QueryDomain.contacts,
              canRead: true,
              canWrite: false,
              canObserve: true,
              canStream: false,
              reason:
                  'simple_query: contacts are read-only via EDS (requires libebook-1.2)',
            ),
            CapabilityDescriptor(
              domain: QueryDomain.media,
              canRead: true,
              canWrite: true,
              canObserve: true,
              canStream: true,
            ),
            CapabilityDescriptor(
              domain: QueryDomain.files,
              canRead: true,
              canWrite: true,
              canObserve: true,
              canStream: true,
            ),
            CapabilityDescriptor(
              domain: QueryDomain.calendar,
              canRead: true,
              canWrite: false,
              canObserve: true,
              canStream: false,
              reason:
                  'simple_query: calendar is read-only via EDS (requires libecal-2.0)',
            ),
            CapabilityDescriptor(
              domain: QueryDomain.messages,
              canRead: false,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason:
                  'simple_query: OS policy does not expose system SMS/MMS APIs cross-platform',
            ),
            CapabilityDescriptor(
              domain: QueryDomain.calls,
              canRead: false,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason:
                  'simple_query: OS policy does not expose system call logs cross-platform',
            ),
            CapabilityDescriptor(
              domain: QueryDomain.platformSpecific,
              canRead: true,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason: 'simple_query: extension methods are read-only currently',
            ),
          ],
          platformExtensions: <String, Object?>{
            'linux.eds': true,
            'linux.tracker': true,
            'linux.xdg': true,
            'batchSemantics': 'sequentialBestEffort',
            'fallbackDomains': <String>['files', 'media'],
            'capabilityPrerequisites': <String, Object?>{
              'files': <String>['platformData.rootPath'],
              'media': <String>['platformData.rootPath'],
            },
            'queryFieldStability': <String, String>{
              'entityType': 'stable',
              'projection': 'stable',
              'platformData': 'platform_extension',
            },
          },
        ),
      );

  static String _localUnsupportedReason(QueryDomain domain) {
    switch (domain) {
      case QueryDomain.messages:
        return 'OS policy does not expose system SMS/MMS APIs';
      case QueryDomain.calls:
        return 'OS policy does not expose system call logs';
      case QueryDomain.contacts:
        return 'Contacts are read-only via EDS; write not supported';
      case QueryDomain.calendar:
        return 'Calendar is read-only via EDS; write not supported';
      case QueryDomain.platformSpecific:
        return 'Use callExtension for platform-specific operations';
      case QueryDomain.media:
      case QueryDomain.files:
        return 'Unsupported domain';
    }
  }

  static bool _nativeDomainSupported(QueryDomain domain) {
    return domain == QueryDomain.contacts ||
        domain == QueryDomain.files ||
        domain == QueryDomain.media ||
        domain == QueryDomain.calendar;
  }

  Future<Map<String, Object?>?> _handleEdsExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listAddressBooks':
        _ensureNoArgs(args, method: 'linux.eds.listAddressBooks');
        return const <String, Object?>{
          'addressBooks': <Map<String, Object?>>[],
        };
      case 'listCalendars':
        _ensureNoArgs(args, method: 'linux.eds.listCalendars');
        return const <String, Object?>{'calendars': <Map<String, Object?>>[]};
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'linux.eds method $method is not supported',
        );
    }
  }

  Future<Map<String, Object?>?> _handleTrackerExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listIndexScopes':
        final limit = args?['limit'];
        if (limit != null && limit is! int) {
          throw invalidQuery(
            'simple_query: linux.tracker.listIndexScopes expects limit as int',
          );
        }
        final scopes = <Map<String, Object?>>[
          <String, Object?>{
            'name': 'cwd',
            'path': Directory.current.path,
          },
        ];
        return <String, Object?>{
          'scopes': (limit is int)
              ? scopes.take(limit).toList(growable: false)
              : scopes,
          'implementation': 'diagnostic_synthetic',
        };
      case 'listGraphNames':
        _ensureNoArgs(args, method: 'linux.tracker.listGraphNames');
        return const <String, Object?>{
          'graphs': <String>[
            'tracker:Documents',
            'tracker:Pictures',
            'tracker:Audio',
            'tracker:Video',
          ],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'linux.tracker method $method is not supported',
        );
    }
  }

  Future<Map<String, Object?>?> _handleXdgExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listIndexScopes':
        final includeTemp = args?['includeTemp'];
        if (includeTemp != null && includeTemp is! bool) {
          throw invalidQuery(
            'simple_query: linux.xdg.listIndexScopes expects includeTemp as bool',
          );
        }
        return <String, Object?>{
          'scopes': <Map<String, Object?>>[
            <String, Object?>{
              'name': 'home',
              'path': Platform.environment['HOME'] ?? Directory.current.path,
            },
            if (includeTemp != false)
              <String, Object?>{
                'name': 'temp',
                'path': Directory.systemTemp.path,
              },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'linux.xdg method $method is not supported',
        );
    }
  }

  void _ensureNoArgs(Map<String, Object?>? args, {required String method}) {
    if (args != null && args.isNotEmpty) {
      throw invalidQuery('simple_query: $method does not accept arguments');
    }
  }
}
