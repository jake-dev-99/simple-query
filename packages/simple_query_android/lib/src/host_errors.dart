import 'package:flutter/services.dart' show PlatformException;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart'
    as iface;

/// The stable prefix every mapped error message carries, per
/// docs/ERROR_MAPPING.md.
const simpleQueryMessagePrefix = 'simple_query: ';

/// Purpose: Name the Pigeon error codes the Android host path can raise.
///
/// The first five are the vocabulary every simple_query bridge sends;
/// the Kotlin side classifies native exceptions into them (`HostErrors.kt`).
/// The last two are raised by Pigeon's generated Dart code itself.
abstract final class HostErrorCodes {
  static const permissionDenied = 'permission-denied';
  static const invalidQuery = 'invalid-query';
  static const unavailable = 'unavailable';
  static const transient = 'transient';
  static const notSupported = 'not-supported';

  /// No host handler answered the Pigeon channel (plugin not attached).
  static const channelError = 'channel-error';

  /// The host returned null for a result Pigeon declares non-null.
  static const nullError = 'null-error';
}

/// Purpose: Run one Pigeon host call so that a host failure reaches callers
/// only as a `SimpleQueryError`, never as a raw `PlatformException`
/// (docs/ERROR_MAPPING.md, UNFY-300).
///
/// @param hostCall performs the Pigeon call.
/// @param domain the request's domain, when known.
/// @param operation the request's operation, when known.
/// @param contentUri the provider URI the call targets, when known.
/// @returns The host call's result.
/// @throws SimpleQueryError mapped by [mapHostError] for every
///   `PlatformException` the call raises, keeping its stack trace.
Future<T> guardHostCall<T>(
  Future<T> Function() hostCall, {
  iface.QueryDomain? domain,
  iface.QueryOperation? operation,
  String? contentUri,
}) async {
  try {
    return await hostCall();
  } on PlatformException catch (error, stackTrace) {
    Error.throwWithStackTrace(
      mapHostError(
        error,
        domain: domain,
        operation: operation,
        contentUri: contentUri,
      ),
      stackTrace,
    );
  }
}

/// Purpose: Convert one host `PlatformException` into the
/// `SimpleQueryError` docs/ERROR_MAPPING.md prescribes.
///
/// @param error the host failure.
/// @param domain the request's domain, when known.
/// @param operation the request's operation, when known.
/// @param contentUri the provider URI the call targets; a URI the host
///   names in its details (e.g. a failed join's) takes precedence.
/// @returns The mapped error. Its `details` keep the host's `code` and
///   `details` (plus `contentUri` and `nativeType` when known), so the
///   original failure is not lost.
/// @throws Nothing.
iface.SimpleQueryError mapHostError(
  PlatformException error, {
  iface.QueryDomain? domain,
  iface.QueryOperation? operation,
  String? contentUri,
}) {
  final nativeDetails = error.details;
  final nativeUri = nativeDetails is Map ? nativeDetails['contentUri'] : null;
  final failingUri = nativeUri is String ? nativeUri : contentUri;
  final nativeType = nativeDetails is Map ? nativeDetails['nativeType'] : null;
  return iface.SimpleQueryError(
    code: _codeFor(error.code),
    message: _messageFor(error, failingUri),
    domain: domain,
    operation: operation,
    details: <String, Object?>{
      if (failingUri != null) 'contentUri': failingUri,
      'code': error.code,
      if (nativeType is String) 'nativeType': nativeType,
      'details': nativeDetails,
    },
  );
}

/// Purpose: Map a host error code onto the closed `SimpleQueryErrorCode` set.
///
/// @param code the `PlatformException.code` the host call raised.
/// @returns The matching code. A code no rule covers maps to
///   `transientFailure`, so `unavailable` keeps meaning only "provider
///   unreachable" (docs/ERROR_MAPPING.md); the raw code stays in
///   `details.code`.
/// @throws Nothing.
iface.SimpleQueryErrorCode _codeFor(String code) => switch (code) {
      HostErrorCodes.permissionDenied =>
        iface.SimpleQueryErrorCode.permissionDenied,
      HostErrorCodes.invalidQuery => iface.SimpleQueryErrorCode.invalidQuery,
      HostErrorCodes.unavailable ||
      HostErrorCodes.channelError =>
        iface.SimpleQueryErrorCode.unavailable,
      HostErrorCodes.notSupported => iface.SimpleQueryErrorCode.notSupported,
      HostErrorCodes.transient ||
      HostErrorCodes.nullError =>
        iface.SimpleQueryErrorCode.transientFailure,
      _ => iface.SimpleQueryErrorCode.transientFailure,
    };

/// Purpose: Give a mapped error the stable `simple_query:` message prefix.
///
/// @param error the host failure.
/// @param contentUri the failing provider URI, when known.
/// @returns The host's message when it already carries the prefix (the
///   Kotlin side always sends one); otherwise a prefixed message naming
///   the call target, the host code, and the host's own message.
/// @throws Nothing.
String _messageFor(PlatformException error, String? contentUri) {
  final nativeMessage = error.message;
  if (nativeMessage != null &&
      nativeMessage.startsWith(simpleQueryMessagePrefix)) {
    return nativeMessage;
  }
  final target = contentUri == null ? 'host call' : 'host call for $contentUri';
  final suffix = nativeMessage == null ? '' : ': $nativeMessage';
  return '$simpleQueryMessagePrefix$target failed (${error.code})$suffix';
}
