import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_ios/simple_query_ios.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

void main() {
  final original = SimpleQueryPlatform.instance;

  tearDown(() {
    SimpleQueryPlatform.instance = original;
  });

  test('registerWith sets platform instance', () {
    SimpleQueryIos.registerWith();
    expect(SimpleQueryPlatform.instance, isA<SimpleQueryIos>());
  });

  test('capabilities expose files/media and restricted domains', () async {
    SimpleQueryIos.registerWith();
    final capabilities = await SimpleQueryPlatform.instance.getCapabilities();

    final files = capabilities.capabilities
        .firstWhere((item) => item.domain == QueryDomain.files);
    final messages = capabilities.capabilities
        .firstWhere((item) => item.domain == QueryDomain.messages);

    expect(files.canRead, isTrue);
    expect(messages.canRead, isFalse);
  });

  test('capability matrix matches iOS current implementation', () async {
    SimpleQueryIos.registerWith();
    final snapshot = await SimpleQueryPlatform.instance.getCapabilities();
    final map = <QueryDomain, CapabilityDescriptor>{
      for (final capability in snapshot.capabilities)
        capability.domain: capability,
    };

    expect(map.length, QueryDomain.values.length);
    expect(map[QueryDomain.files]!.canRead, isTrue);
    expect(map[QueryDomain.files]!.canWrite, isTrue);
    expect(map[QueryDomain.files]!.canObserve, isTrue);
    expect(map[QueryDomain.files]!.canStream, isTrue);

    expect(map[QueryDomain.media]!.canRead, isTrue);
    expect(map[QueryDomain.media]!.canWrite, isTrue);
    expect(map[QueryDomain.media]!.canObserve, isTrue);
    expect(map[QueryDomain.media]!.canStream, isTrue);

    for (final domain in <QueryDomain>[
      QueryDomain.contacts,
      QueryDomain.calendar,
    ]) {
      final capability = map[domain]!;
      expect(capability.canRead, isTrue);
      expect(capability.canWrite, isFalse);
      expect(capability.canObserve, isTrue);
      expect(capability.canStream, isFalse);
      expect(capability.reason, isNotNull);
    }

    for (final domain in <QueryDomain>[
      QueryDomain.messages,
      QueryDomain.calls,
    ]) {
      final capability = map[domain]!;
      expect(capability.canRead, isFalse);
      expect(capability.canWrite, isFalse);
      expect(capability.canObserve, isFalse);
      expect(capability.canStream, isFalse);
      expect(capability.reason, isNotNull);
      expect(capability.reason, contains('simple_query:'));
    }

    final platformSpecific = map[QueryDomain.platformSpecific]!;
    expect(platformSpecific.canRead, isTrue);
    expect(platformSpecific.canWrite, isFalse);
    expect(platformSpecific.canObserve, isFalse);
    expect(platformSpecific.canStream, isFalse);
    final prerequisites =
        (snapshot.platformExtensions['capabilityPrerequisites']
            as Map<String, Object?>)['files'] as List<Object?>;
    expect(
      prerequisites.any((item) => item.toString().contains('rootPath')),
      isTrue,
    );
  });

  test('files query returns typed records', () async {
    SimpleQueryIos.registerWith();

    final temp =
        await Directory.systemTemp.createTemp('simple_query_ios_query_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('hello');

    final result = await SimpleQueryPlatform.instance.query(
      QueryRequest(
        domain: QueryDomain.files,
        platformData: <String, Object?>{'rootPath': temp.path},
      ),
    );

    expect(result.records, isNotEmpty);
    expect(
      QueryDomainContracts.hasRequiredKeys(
        domain: QueryDomain.files,
        record: result.records.first,
      ),
      isTrue,
    );
    expect(
      QueryDomainContracts.unknownKeys(
        domain: QueryDomain.files,
        record: result.records.first,
      ),
      isEmpty,
    );

    await temp.delete(recursive: true);
  });

  test('media query returns typed records', () async {
    SimpleQueryIos.registerWith();

    final temp =
        await Directory.systemTemp.createTemp('simple_query_ios_media_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.jpg');
    await file.writeAsBytes(<int>[1, 2, 3, 4]);

    final result = await SimpleQueryPlatform.instance.query(
      QueryRequest(
        domain: QueryDomain.media,
        platformData: <String, Object?>{'rootPath': temp.path},
      ),
    );

    expect(result.records, isNotEmpty);
    expect(
      QueryDomainContracts.hasRequiredKeys(
        domain: QueryDomain.media,
        record: result.records.first,
      ),
      isTrue,
    );
    expect(
      QueryDomainContracts.unknownKeys(
        domain: QueryDomain.media,
        record: result.records.first,
      ),
      isEmpty,
    );

    await temp.delete(recursive: true);
  });

  test('update mutation writes file content for files domain', () async {
    SimpleQueryIos.registerWith();

    final temp =
        await Directory.systemTemp.createTemp('simple_query_ios_update_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('before');

    final result = await SimpleQueryPlatform.instance.mutate(
      MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.update,
        values: <String, Object?>{
          'path': file.path,
          'content': 'after',
        },
        platformData: <String, Object?>{'rootPath': temp.path},
      ),
    );

    expect(result.affectedCount, 1);
    expect(await file.readAsString(), 'after');
    await temp.delete(recursive: true);
  });

  test('restricted domain throws deterministic notSupported error', () {
    SimpleQueryIos.registerWith();

    expect(
      () => SimpleQueryPlatform.instance.query(
        const QueryRequest(domain: QueryDomain.messages),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.notSupported),
      ),
    );
  });

  test('observe restricted domain emits deterministic notSupported error',
      () async {
    SimpleQueryIos.registerWith();

    await expectLater(
      SimpleQueryPlatform.instance.observe(
        const ObserveRequest(domain: QueryDomain.calls),
      ),
      emitsError(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.notSupported),
      ),
    );
  });

  test('observe files supports subscribe and cancel lifecycle', () async {
    SimpleQueryIos.registerWith();

    final temp =
        await Directory.systemTemp.createTemp('simple_query_ios_observe_');
    final stream = SimpleQueryPlatform.instance.observe(
      ObserveRequest(
        domain: QueryDomain.files,
        platformData: <String, Object?>{'rootPath': temp.path},
        pollingInterval: const Duration(milliseconds: 100),
      ),
    );

    final sub = stream.listen((_) {});
    await Future<void>.delayed(const Duration(milliseconds: 250));
    await sub.cancel();

    await temp.delete(recursive: true);
  });

  test('openBinary and closeBinary lifecycle succeeds for files domain',
      () async {
    SimpleQueryIos.registerWith();

    final temp =
        await Directory.systemTemp.createTemp('simple_query_ios_binary_');
    final file = File('${temp.path}${Platform.pathSeparator}blob.txt');
    await file.writeAsString('payload');

    final handle = await SimpleQueryPlatform.instance.openBinary(
      BinaryRequest(
        domain: QueryDomain.files,
        recordId: file.path,
        platformData: <String, Object?>{'rootPath': temp.path},
      ),
    );

    expect(handle.handleId, isNotEmpty);
    expect(handle.localPath, file.path);
    await SimpleQueryPlatform.instance.closeBinary(handle.handleId);

    await temp.delete(recursive: true);
  });

  test('callExtension supports ios.photos fetchAssetResources', () async {
    SimpleQueryIos.registerWith();

    final result = await SimpleQueryPlatform.instance.callExtension(
      namespace: 'ios.photos',
      method: 'fetchAssetResources',
      args: <String, Object?>{'rootPath': Directory.current.path},
    );

    expect(result, isNotNull);
    expect(result!['resources'], isA<List<Object?>>());
  });

  test('callExtension supports ios.photos listMediaTypes', () async {
    SimpleQueryIos.registerWith();

    final result = await SimpleQueryPlatform.instance.callExtension(
      namespace: 'ios.photos',
      method: 'listMediaTypes',
    );

    expect(result, isNotNull);
    expect(result!['mediaTypes'], isA<List<Object?>>());
  });

  test('callExtension unknown method returns deterministic notSupported',
      () async {
    SimpleQueryIos.registerWith();

    expect(
      () => SimpleQueryPlatform.instance.callExtension(
        namespace: 'ios.photos',
        method: 'unknownMethod',
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.notSupported)
            .having((e) => e.message, 'message', contains('simple_query:')),
      ),
    );
  });

  test('callExtension invalid args return invalidQuery', () async {
    SimpleQueryIos.registerWith();

    expect(
      () => SimpleQueryPlatform.instance.callExtension(
        namespace: 'ios.photos',
        method: 'fetchAssetResources',
        args: <String, Object?>{'limit': 'bad'},
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
            .having((e) => e.message, 'message', contains('simple_query:')),
      ),
    );
  });

  test('files query with explicit rootPath succeeds', () async {
    SimpleQueryIos.registerWith();
    final tmpDir = Directory.systemTemp.createTempSync('simple_query_ios_');
    addTearDown(() => tmpDir.deleteSync(recursive: true));

    final result = await SimpleQueryPlatform.instance.query(
      QueryRequest(
        domain: QueryDomain.files,
        platformData: <String, Object?>{'rootPath': tmpDir.path},
      ),
    );

    expect(result.records, isEmpty);
  });
}
