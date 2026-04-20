import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'base_non_android_platform.dart';

/// Configuration that differs between iOS and macOS.
class DarwinPlatformConfig {
  const DarwinPlatformConfig({
    required this.isCurrentPlatform,
    required this.prefix,
    required this.fallbackSource,
    required this.contactsExtensionMethods,
    this.defaultRootPath,
  });

  final bool Function() isCurrentPlatform;

  /// Namespace prefix for extensions (e.g. 'ios' or 'macos').
  final String prefix;

  /// Polling source name.
  final String fallbackSource;

  /// Default root path for filesystem fallback when the caller does not
  /// provide `platformData.rootPath`. Null means rootPath is required.
  final String? defaultRootPath;

  /// Platform-specific contacts extension methods beyond the shared ones.
  final Future<Map<String, Object?>?> Function({
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
  }) contactsExtensionMethods;
}

/// Shared Darwin (iOS/macOS) implementation. Subclasses pass their
/// Pigeon host API method references via [NonAndroidHostBindings].
abstract class DarwinQueryPlatform extends BaseNonAndroidSimpleQueryPlatform {
  /// Constructor for platform packages. Pass the Pigeon-generated host API's
  /// methods via [bindings] to avoid a direct dependency on generated code.
  DarwinQueryPlatform.create({
    required NonAndroidHostBindings bindings,
    required DarwinPlatformConfig config,
  }) : _config = config {
    initializeBase(
      isCurrentPlatform: config.isCurrentPlatform,
      fallbackSource: config.fallbackSource,
      unsupportedReasonFor: _localUnsupportedReason,
      nativeDomainSupported: _nativeDomainSupported,
      defaultRootPath: config.defaultRootPath,
      bindings: bindings,
    );
  }

  final DarwinPlatformConfig _config;

  String get _prefix => _config.prefix;

  @override
  CapabilitySnapshot get defaultCapabilities =>
      RuntimeContractValidation.validateCapabilitySnapshot(
        CapabilitySnapshot(
          capabilities: <CapabilityDescriptor>[
            const CapabilityDescriptor(
              domain: QueryDomain.contacts,
              canRead: true,
              canWrite: false,
              canObserve: true,
              canStream: false,
              reason:
                  'simple_query: contacts are read-only; write support requires native implementation',
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.media,
              canRead: true,
              canWrite: true,
              canObserve: true,
              canStream: true,
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.files,
              canRead: true,
              canWrite: true,
              canObserve: true,
              canStream: true,
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.calendar,
              canRead: true,
              canWrite: false,
              canObserve: true,
              canStream: false,
              reason:
                  'simple_query: calendar is read-only; write support requires native implementation',
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.messages,
              canRead: false,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason:
                  'simple_query: OS policy does not expose system SMS/MMS APIs cross-platform',
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.calls,
              canRead: false,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason:
                  'simple_query: OS policy does not expose system call logs cross-platform',
            ),
            const CapabilityDescriptor(
              domain: QueryDomain.platformSpecific,
              canRead: true,
              canWrite: false,
              canObserve: false,
              canStream: false,
              reason: 'simple_query: extension methods are read-only currently',
            ),
          ],
          platformExtensions: <String, Object?>{
            '$_prefix.contacts': true,
            '$_prefix.calendar': true,
            '$_prefix.photos': true,
            'batchSemantics': 'sequentialBestEffort',
            'fallbackDomains': const <String>['files', 'media'],
            'capabilityPrerequisites': const <String, Object?>{
              'files': <String>[
                'platformData.rootPath (optional, defaults to platform directory)'
              ],
              'media': <String>[
                'platformData.rootPath (optional, defaults to platform directory)'
              ],
            },
            'queryFieldStability': const <String, String>{
              'entityType': 'stable',
              'projection': 'stable',
              'platformData': 'platform_extension',
            },
          },
        ),
      );

  @override
  Future<Map<String, Object?>?> handleLocalExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    if (namespace == '$_prefix.contacts') {
      return _handleContactsExtension(method: method, args: args);
    }
    if (namespace == '$_prefix.calendar') {
      return _handleCalendarExtension(method: method, args: args);
    }
    if (namespace == '$_prefix.photos') {
      return _handlePhotosExtension(method: method, args: args);
    }
    throw notSupported(
      operation: QueryOperation.read,
      domain: QueryDomain.platformSpecific,
      reason:
          'Extension namespace $namespace is not supported on $_prefix backend',
    );
  }

  void onObserveEventFromNative(
      String observerId, Map<String?, Object?> event) {
    handleObserveEvent(observerId, event);
  }

  static String _localUnsupportedReason(QueryDomain domain) {
    switch (domain) {
      case QueryDomain.messages:
        return 'OS policy does not expose system SMS/MMS APIs';
      case QueryDomain.calls:
        return 'OS policy does not expose system call logs';
      case QueryDomain.contacts:
        return 'Contacts are read-only via native backend; write not supported';
      case QueryDomain.calendar:
        return 'Calendar is read-only via native backend; write not supported';
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
    // Shared method.
    if (method == 'listGroups') {
      _ensureNoArgs(args, method: '$_prefix.contacts.listGroups');
      return const <String, Object?>{
        'groups': <Map<String, Object?>>[],
        'implementation': 'diagnostic_synthetic',
      };
    }
    // Delegate platform-specific methods.
    return _config.contactsExtensionMethods(
      method: method,
      args: args,
      ensureNoArgs: _ensureNoArgs,
      notSupportedBuilder: notSupported,
      invalidQueryBuilder: invalidQuery,
    );
  }

  Future<Map<String, Object?>?> _handleCalendarExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'listCalendars':
        _ensureNoArgs(args, method: '$_prefix.calendar.listCalendars');
        return const <String, Object?>{
          'calendars': <Map<String, Object?>>[],
          'implementation': 'diagnostic_synthetic',
        };
      case 'getDefaultTimeZone':
        _ensureNoArgs(args, method: '$_prefix.calendar.getDefaultTimeZone');
        return <String, Object?>{
          'timeZone': DateTime.now().timeZoneName,
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: '$_prefix.calendar method $method is not supported',
        );
    }
  }

  Future<Map<String, Object?>?> _handlePhotosExtension({
    required String method,
    Map<String, Object?>? args,
  }) async {
    switch (method) {
      case 'fetchAssetResources':
        final limit = args?['limit'];
        if (limit != null && limit is! int) {
          throw invalidQuery(
            'simple_query: $_prefix.photos.fetchAssetResources expects limit as int',
          );
        }
        final rootPathArg = args?['rootPath'];
        if (rootPathArg != null && rootPathArg is! String) {
          throw invalidQuery(
            'simple_query: $_prefix.photos.fetchAssetResources expects rootPath as String',
          );
        }
        final result = await localFallback.query(
          QueryRequest(
            domain: QueryDomain.media,
            platformData: <String, Object?>{
              if (rootPathArg != null) 'rootPath': rootPathArg,
            },
          ),
        );
        final resources = result.records
            .map(
              (record) => <String, Object?>{
                'id': record['uriOrPath'] ?? '',
                'uriOrPath': record['uriOrPath'] ?? '',
                'mimeType': record['mimeType'],
                'size': record['size'],
              },
            )
            .toList(growable: false);
        return <String, Object?>{
          'resources': (limit is int)
              ? resources.take(limit).toList(growable: false)
              : resources,
          'implementation': 'filesystem_fallback',
        };
      case 'listMediaTypes':
        _ensureNoArgs(args, method: '$_prefix.photos.listMediaTypes');
        return const <String, Object?>{
          'mediaTypes': <String>['image', 'video', 'audio'],
          'implementation': 'diagnostic_synthetic',
        };
      default:
        throw notSupported(
          operation: QueryOperation.read,
          domain: QueryDomain.platformSpecific,
          reason: '$_prefix.photos method $method is not supported',
        );
    }
  }

  void _ensureNoArgs(Map<String, Object?>? args, {required String method}) {
    if (args != null && args.isNotEmpty) {
      throw invalidQuery('simple_query: $method does not accept arguments');
    }
  }
}
