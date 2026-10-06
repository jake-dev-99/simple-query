import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_linux/simple_query_linux.dart';
import 'package:simple_query_linux/src/generated/native_query.g.dart' as native;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

const _batchChannel =
    'dev.flutter.pigeon.simple_query_linux.NativeQueryHostApi.batch';
const _observeStartChannel =
    'dev.flutter.pigeon.simple_query_linux.NativeQueryHostApi.observeStart';
const _observeStopChannel =
    'dev.flutter.pigeon.simple_query_linux.NativeQueryHostApi.observeStop';
const _observeEventChannel =
    'dev.flutter.pigeon.simple_query_linux.NativeQueryFlutterApi.onObserveEvent';

/// Purpose: Read one exact native codec fixture without altering its bytes.
///
/// @param directory contains native outputs.
/// @param name selects one fixture.
/// @returns The fixture as a binary messenger payload.
/// @throws [FileSystemException] when the fixture is missing or unreadable.
ByteData _fixture(String directory, String name) => ByteData.sublistView(
      File('$directory${Platform.pathSeparator}$name').readAsBytesSync(),
    );

/// Purpose: Replay native responses through generated channels and the bridge.
///
/// @param fixtureDirectory contains bytes captured from the generated C++ API.
/// @returns A future that completes after both payloads cross the boundary.
/// @throws The underlying test failure or codec error.
Future<void> _verifyNativeContracts(String fixtureDirectory) async {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const codec = native.NativeQueryHostApi.pigeonChannelCodec;
  messenger.setMockMessageHandler(
    _batchChannel,
    (_) async => _fixture(fixtureDirectory, 'batch_response.bin'),
  );
  messenger.setMockMessageHandler(
    _observeStartChannel,
    (_) async => codec.encodeMessage(<Object?>['linux_observer_1']),
  );
  messenger.setMockMessageHandler(
    _observeStopChannel,
    (_) async => codec.encodeMessage(<Object?>[]),
  );

  final platform = SimpleQueryLinux();
  addTearDown(() async {
    await platform.dispose();
    native.NativeQueryFlutterApi.setUp(null, binaryMessenger: messenger);
    messenger.setMockMessageHandler(_batchChannel, null);
    messenger.setMockMessageHandler(_observeStartChannel, null);
    messenger.setMockMessageHandler(_observeStopChannel, null);
  });

  final batch = await platform.batch(
    const BatchRequest(
      operations: <MutationRequest>[
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.insert,
        ),
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.insert,
          values: <String, Object?>{'path': 'batch-success.txt'},
        ),
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.insert,
        ),
      ],
    ),
  );
  expect(batch.results, hasLength(3));
  expect(batch.results[1].affectedCount, 1);
  const expectedCodes = <int, String>{
    0: 'unavailable',
    2: 'invalidQuery',
  };
  for (final index in <int>[0, 2]) {
    final metadata = batch.results[index].metadata!;
    expect(metadata['batchSemantics'], 'sequentialBestEffort');
    expect(metadata['implementation'], 'native_linux');
    final error = metadata['error']! as Map<Object?, Object?>;
    expect(error['code'], expectedCodes[index]);
    expect(error['message'], startsWith('simple_query:'));
    expect(error['domain'], 'files');
    expect(error['operation'], 'write');
  }

  final eventFuture = platform
      .observe(const ObserveRequest(domain: QueryDomain.files))
      .first
      .timeout(const Duration(seconds: 2));
  await messenger.platformMessagesFinished;
  final eventBytes = _fixture(fixtureDirectory, 'observe_event.bin');
  final reply = await messenger.handlePlatformMessage(
    _observeEventChannel,
    eventBytes,
    null,
  );
  final event = await eventFuture;
  final wireMessage = native.NativeQueryFlutterApi.pigeonChannelCodec
      .decodeMessage(eventBytes)! as List<Object?>;
  final wireEvent = wireMessage[1]! as Map<Object?, Object?>;
  final wireTimestamp = wireEvent['timestamp']! as String;

  expect(
    native.NativeQueryFlutterApi.pigeonChannelCodec.decodeMessage(reply),
    isEmpty,
  );
  expect(
    wireTimestamp,
    matches(RegExp(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$')),
  );
  expect(event.domain, QueryDomain.files);
  expect(event.changeType, ObserveChangeType.unknown);
  expect(event.ids, isNotEmpty);
  expect(event.source, 'linux-host');
  expect(event.timestamp.isUtc, isTrue);
  expect(event.timestamp, DateTime.parse(wireTimestamp).toUtc());
}

/// Purpose: Register the native-to-Dart contract replay test.
///
/// @returns Nothing.
/// @throws Nothing directly; the registered test reports contract failures.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const fixtureDirectory =
      String.fromEnvironment('SIMPLE_QUERY_CONTRACT_FIXTURE_DIR');

  test(
    'native codec fixtures cross Linux and shared Dart boundaries',
    () => _verifyNativeContracts(fixtureDirectory),
    skip: fixtureDirectory.isEmpty
        ? 'Run through tool/run_native_tests.sh to generate native fixtures.'
        : false,
  );
}
