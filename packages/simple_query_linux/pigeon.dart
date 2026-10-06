import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/src/generated/native_query.g.dart',
    dartPackageName: 'simple_query_linux',
    gobjectHeaderOut: 'linux/native_query.g.h',
    gobjectSourceOut: 'linux/native_query.g.cc',
    gobjectOptions: GObjectOptions(
      module: 'Sqlq',
    ),
  ),
)

/// Purpose: Defines the Linux host operations used by the shared Dart bridge.
///
/// @throws PlatformException when Linux returns a structured native error.
@HostApi()
abstract class NativeQueryHostApi {
  /// Purpose: Reports Linux domain and extension availability.
  ///
  /// @returns A capability payload understood by the shared bridge.
  /// @throws PlatformException when capability discovery is unavailable.
  Map<String?, Object?> getCapabilities();

  /// Purpose: Executes a read request against a supported Linux domain.
  ///
  /// @param request is the serialized cross-platform query request.
  /// @returns A serialized page of matching records.
  /// @throws PlatformException for invalid, unsupported, or unavailable reads.
  Map<String?, Object?> query(Map<String?, Object?> request);

  /// Purpose: Executes one filesystem or media mutation on Linux.
  ///
  /// @param request is the serialized cross-platform mutation request.
  /// @returns The serialized mutation result.
  /// @throws PlatformException for invalid, unsupported, or unavailable writes.
  Map<String?, Object?> mutate(Map<String?, Object?> request);

  /// Purpose: Executes an ordered group of Linux mutations.
  ///
  /// @param request contains the serialized mutation operations.
  /// @returns The serialized per-operation results.
  /// @throws PlatformException when the batch request itself is invalid.
  Map<String?, Object?> batch(Map<String?, Object?> request);

  /// Purpose: Starts polling a supported Linux domain for changes.
  ///
  /// @param request is the serialized observation request.
  /// @returns The opaque identifier used to stop this observer.
  /// @throws PlatformException when the domain cannot be observed.
  String observeStart(Map<String?, Object?> request);

  /// Purpose: Stops one Linux observer and cancels its pending deliveries.
  ///
  /// @param observerId identifies the observer returned by observeStart.
  /// @returns Nothing.
  /// @throws PlatformException when native cleanup fails.
  void observeStop(String observerId);

  /// Purpose: Opens a supported Linux file as binary content.
  ///
  /// @param request identifies the file or media record.
  /// @returns A serialized binary-content handle.
  /// @throws PlatformException when the resource is invalid or unavailable.
  Map<String?, Object?> openBinary(Map<String?, Object?> request);

  /// Purpose: Releases a previously opened Linux binary handle.
  ///
  /// @param handleId identifies the native binary handle.
  /// @returns Nothing.
  /// @throws PlatformException when native cleanup fails.
  void closeBinary(String handleId);

  /// Purpose: Calls a namespaced Linux-only diagnostic extension.
  ///
  /// @param namespace_ selects the registered Linux extension namespace.
  /// @param method selects the extension operation.
  /// @param args contains optional extension-specific arguments.
  /// @returns The serialized extension response, or null when appropriate.
  /// @throws PlatformException for invalid or unsupported extension calls.
  Map<String?, Object?>? callExtension(
    String namespace_,
    String method,
    Map<String?, Object?>? args,
  );
}

/// Purpose: Delivers native Linux observation events to the Dart bridge.
@FlutterApi()
abstract class NativeQueryFlutterApi {
  /// Purpose: Reports one completed native observation change.
  ///
  /// @param observerId identifies the active native observer.
  /// @param event is the serialized cross-platform observation event.
  /// @returns Nothing.
  /// @throws PlatformException when Dart rejects the event delivery.
  void onObserveEvent(String observerId, Map<String?, Object?> event);
}
