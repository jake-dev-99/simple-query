import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/src/generated/native_query.g.dart',
    dartPackageName: 'simple_query_windows',
    cppHeaderOut: 'windows/native_query.g.h',
    cppSourceOut: 'windows/native_query.g.cpp',
    cppOptions: CppOptions(
      namespace: 'simple_query_windows',
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
  /// Purpose: Calls a namespaced Windows-only extension.
  ///
  /// @param extensionNamespace selects the registered Windows extension namespace.
  /// It is not named `namespace`, a C++ keyword, and carries no trailing
  /// underscore, which Pigeon would turn into a reserved C++ identifier.
  Map<String?, Object?>? callExtension(
    String extensionNamespace,
    String method,
    Map<String?, Object?>? args,
  );
}

@FlutterApi()
abstract class NativeQueryFlutterApi {
  void onObserveEvent(String observerId, Map<String?, Object?> event);
}
