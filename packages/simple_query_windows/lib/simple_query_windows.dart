library;

import 'dart:io';

import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_shared/simple_query_shared.dart';
import 'package:simple_query_windows/src/generated/native_query.g.dart'
    as native;

class SimpleQueryWindows extends BaseNonAndroidSimpleQueryPlatform
    implements native.NativeQueryFlutterApi {
  SimpleQueryWindows() {
    initializeBase(
      isCurrentPlatform: () => Platform.isWindows,
      fallbackSource: 'simple_query_windows.polling',
      unsupportedReasonFor: _localUnsupportedReason,
      nativeDomainSupported: _nativeDomainSupported,
      defaultRootPath: Platform.isWindows
          ? Platform.environment['USERPROFILE']
          : null,
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
    SimpleQueryPlatform.instance = SimpleQueryWindows();
  }

  final native.NativeQueryHostApi _nativeHostApi = native.NativeQueryHostApi();
  @override
  Future<Map<String, Object?>?> handleLocalExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (namespace) {
      case 'windows.contacts':
        return _handleContactsExtension(method: method, args: args);
      case 'windows.calendar':
        return _handleCalendarExtension(method: method, args: args);
      case 'windows.storage':
        return _handleStorageExtension(method: method, args: args);
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason:
              'Extension namespace $namespace is not supported on Windows backend',
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
                  'simple_query: contacts are read-only via WinRT ContactManager',
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
                  'simple_query: calendar is read-only via WinRT AppointmentManager',
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
              reason:
                  'simple_query: extension methods are read-only currently',
            ),
          ],
          platformExtensions: <String, Object?>{
            'windows.contacts': true,
            'windows.calendar': true,
            'windows.storage': true,
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
        return 'Contacts are read-only via WinRT; write not supported';
      case QueryDomain.calendar:
        return 'Calendar is read-only via WinRT; write not supported';
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

  Future<Map<String, Object?>?> _handleContactsExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listStores':
        _ensureNoArgs(args, method: 'windows.contacts.listStores');
        return <String, Object?>{
          'stores': <Map<String, Object?>>[
            <String, Object?>{
              'id': 'default',
              'name': 'Default Contact Store',
            },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'windows.contacts method $method is not supported',
        );
    }
  }

  Future<Map<String, Object?>?> _handleCalendarExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listCalendars':
        _ensureNoArgs(args, method: 'windows.calendar.listCalendars');
        return const <String, Object?>{
          'calendars': <Map<String, Object?>>[],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'windows.calendar method $method is not supported',
        );
    }
  }

  Future<Map<String, Object?>?> _handleStorageExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'resolveKnownFolders':
        final includeTemp = args?['includeTemp'];
        if (includeTemp != null && includeTemp is! bool) {
          throw invalidQuery(
            'simple_query: windows.storage.resolveKnownFolders expects includeTemp as bool',
          );
        }
        return <String, Object?>{
          'folders': <Map<String, Object?>>[
            <String, Object?>{
              'name': 'current',
              'path': Directory.current.path,
            },
            if (includeTemp != false)
              <String, Object?>{
                'name': 'temp',
                'path': Directory.systemTemp.path,
              },
          ],
          'implementation': 'diagnostic_synthetic',
        };
      case 'listLibraries':
        _ensureNoArgs(args, method: 'windows.storage.listLibraries');
        return const <String, Object?>{
          'libraries': <String>[
            'documents',
            'music',
            'pictures',
            'videos',
          ],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: 'windows.storage method $method is not supported',
        );
    }
  }

  void _ensureNoArgs(Map<String, Object?>? args, {required String method}) {
    if (args != null && args.isNotEmpty) {
      throw invalidQuery('simple_query: $method does not accept arguments');
    }
  }
}
