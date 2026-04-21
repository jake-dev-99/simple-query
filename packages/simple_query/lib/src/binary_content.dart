import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

import 'simple_query_api.dart';

/// A safely-closeable handle to an open binary resource (e.g. a photo or
/// file body opened via [SimpleQuery.openBinaryContent]).
///
/// Wraps the wire-format [BinaryContentHandle] from the platform layer
/// with its lifecycle: callers never have to thread a bare `handleId`
/// string through their code or remember the matching
/// [SimpleQuery.closeBinary] call.
///
/// ```dart
/// final content = await SimpleQuery.instance.openBinaryContent(request);
/// try {
///   final bytes = await File(content.localPath).readAsBytes();
///   // ...
/// } finally {
///   await content.close();
/// }
/// ```
///
/// Or use the scoped helper [SimpleQuery.withBinaryContent] which closes
/// the content automatically even on error.
///
/// [close] is idempotent — a second call is a no-op.
class BinaryContent {
  BinaryContent._(this._handle, this._facade);

  /// Wraps a raw [handle] returned by [SimpleQuery.openBinary] with the
  /// facade reference needed to close it. Used internally by
  /// [SimpleQuery.openBinaryContent] and [SimpleQuery.withBinaryContent];
  /// tests that want to construct a [BinaryContent] around a canned
  /// [BinaryContentHandle] can also use this constructor.
  factory BinaryContent.fromHandle({
    required BinaryContentHandle handle,
    required SimpleQuery facade,
  }) =>
      BinaryContent._(handle, facade);

  final BinaryContentHandle _handle;
  final SimpleQuery _facade;
  bool _closed = false;

  /// Opaque identifier the platform uses to track this open resource.
  /// Most callers should not need this — prefer [localPath] for reading.
  String get handleId => _handle.handleId;

  /// Filesystem path where the binary content has been materialised.
  /// Read-only; do not delete or modify out from under the platform.
  String get localPath => _handle.localPath;

  /// Media type (e.g. `image/jpeg`) when the platform reports one.
  String? get mimeType => _handle.mimeType;

  /// Size in bytes when the platform reports one.
  int? get size => _handle.size;

  /// Platform-specific metadata (e.g. EXIF orientation, MMS part headers).
  Map<String, Object?>? get metadata => _handle.metadata;

  /// True after [close] has been called at least once.
  bool get isClosed => _closed;

  /// The raw wire-format handle. Most callers should not need this — use
  /// the typed getters above. Exposed for advanced interop with the
  /// platform-interface layer.
  BinaryContentHandle get rawHandle => _handle;

  /// Releases the platform-side resource. Idempotent.
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    await _facade.closeBinary(_handle.handleId);
  }
}
