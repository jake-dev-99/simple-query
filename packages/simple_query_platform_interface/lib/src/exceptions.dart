import 'models.dart';

/// A machine-readable code that identifies why a query or mutation failed.
enum SimpleQueryErrorCode {
  /// The requested operation is not available on this platform.
  notSupported,

  /// The app lacks permission to access the requested data.
  permissionDenied,

  /// The data source is temporarily unreachable.
  unavailable,

  /// The query or mutation was malformed or contained invalid parameters.
  invalidQuery,

  /// A temporary failure occurred; retrying may succeed.
  transientFailure,
}

/// An error thrown when a simple_query operation fails.
///
/// Check [code] to determine the failure reason and decide how to recover.
class SimpleQueryError implements Exception {
  const SimpleQueryError({
    required this.code,
    required this.message,
    this.domain,
    this.operation,
    this.details,
  });

  final SimpleQueryErrorCode code;
  final String message;
  final QueryDomain? domain;
  final QueryOperation? operation;
  final Map<String, Object?>? details;

  @override
  String toString() {
    final segments = <String>['SimpleQueryError(${code.name}): $message'];
    if (domain != null) {
      segments.add('domain=${domain!.name}');
    }
    if (operation != null) {
      segments.add('operation=${operation!.name}');
    }
    return segments.join(' ');
  }
}
