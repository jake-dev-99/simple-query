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
@HostApi()
abstract class NativeQueryHostApi {
  Map<String?, Object?> getCapabilities();
  Map<String?, Object?> query(Map<String?, Object?> request);
  Map<String?, Object?> mutate(Map<String?, Object?> request);
  Map<String?, Object?> batch(Map<String?, Object?> request);
  String observeStart(Map<String?, Object?> request);
  void observeStop(String observerId);
  Map<String?, Object?> openBinary(Map<String?, Object?> request);
  void closeBinary(String handleId);
  Map<String?, Object?>? callExtension(
    String namespace_,
    String method,
    Map<String?, Object?>? args,
  );
}

@FlutterApi()
abstract class NativeQueryFlutterApi {
  void onObserveEvent(String observerId, Map<String?, Object?> event);
}
