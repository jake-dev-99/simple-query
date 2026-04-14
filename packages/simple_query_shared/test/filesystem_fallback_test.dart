import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_shared/simple_query_shared.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

void main() {
  late LocalFileSystemFallback fallback;

  setUp(() {
    fallback = LocalFileSystemFallback(
      source: 'test.polling',
      unsupportedReasonFor: (domain) => 'unsupported ${domain.name}',
    );
  });

  tearDown(() {
    fallback.dispose();
  });

  test('query returns filesystem fallback metadata', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_query_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('hello');

    final result = await fallback.query(
      QueryRequest(
        domain: QueryDomain.files,
        platformData: <String, Object?>{'rootPath': temp.path},
      ),
    );

    expect(result.records, hasLength(1));
    expect(result.metadata?['implementation'], 'filesystem_fallback');
    await temp.delete(recursive: true);
  });

  test('batch executes with sequential best effort semantics', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_batch_');
    final first = File('${temp.path}${Platform.pathSeparator}first.txt');
    final second = File('${temp.path}${Platform.pathSeparator}second.txt');
    await first.writeAsString('before-1');
    await second.writeAsString('before-2');

    final result = await fallback.batch(
      BatchRequest(
        platformData: <String, Object?>{'rootPath': temp.path},
        operations: <MutationRequest>[
          MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.update,
            values: <String, Object?>{
              'path': first.path,
              'content': 'after-1',
            },
          ),
          const MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.insert,
            values: <String, Object?>{},
          ),
          MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.update,
            values: <String, Object?>{
              'path': second.path,
              'content': 'after-2',
            },
          ),
        ],
      ),
    );

    expect(result.results, hasLength(3));
    expect(result.results.first.metadata?['batchSemantics'],
        'sequentialBestEffort');
    expect(
      (result.results[1].metadata?['error'] as Map<String, Object?>?)?['code'],
      'invalidQuery',
    );
    expect(await first.readAsString(), 'after-1');
    expect(await second.readAsString(), 'after-2');
    await temp.delete(recursive: true);
  });

  test('observe emits filesystem fallback metadata', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_observe_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('before');

    final eventFuture = fallback
        .observe(
          ObserveRequest(
            domain: QueryDomain.files,
            platformData: <String, Object?>{'rootPath': temp.path},
            pollingInterval: const Duration(milliseconds: 50),
          ),
        )
        .first;

    await Future<void>.delayed(const Duration(milliseconds: 80));
    await file.writeAsString('after');

    final event = await eventFuture;
    expect(event.metadata?['implementation'], 'filesystem_fallback');
    await temp.delete(recursive: true);
  });

  test('closeBinary is idempotent for unknown handles', () async {
    await fallback.closeBinary('missing');
  });

  test('query without rootPath returns invalidQuery', () async {
    expect(
      () => fallback.query(const QueryRequest(domain: QueryDomain.files)),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );
  });

  test('openBinary without rootPath returns invalidQuery', () async {
    expect(
      () => fallback.openBinary(
        const BinaryRequest(
          domain: QueryDomain.files,
          recordId: '/tmp/file.txt',
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );
  });

  test('observe without rootPath emits invalidQuery error', () async {
    await expectLater(
      fallback.observe(const ObserveRequest(domain: QueryDomain.files)),
      emitsError(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );
  });

  test('filesystem exceptions are normalized to unavailable', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_fs_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('before');

    expect(
      () => fallback.mutate(
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.update,
          values: <String, Object?>{
            'path': file.path,
            'newPath': temp.path,
          },
          platformData: <String, Object?>{'rootPath': temp.path},
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.unavailable),
      ),
    );

    await temp.delete(recursive: true);
  });

  test('insert rejects absolute paths outside rootPath', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_guard_');
    final outside = await Directory.systemTemp.createTemp('sq_fallback_out_');

    await expectLater(
      fallback.mutate(
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.insert,
          values: <String, Object?>{
            'path': '${outside.path}${Platform.pathSeparator}escape.txt',
            'content': 'escape',
          },
          platformData: <String, Object?>{'rootPath': temp.path},
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );

    await temp.delete(recursive: true);
    await outside.delete(recursive: true);
  });

  test('update rejects relative escape via newPath', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_move_');
    final file = File('${temp.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('before');

    await expectLater(
      fallback.mutate(
        MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.update,
          values: <String, Object?>{
            'path': file.path,
            'newPath': '..${Platform.pathSeparator}escape.txt',
          },
          platformData: <String, Object?>{'rootPath': temp.path},
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );

    await temp.delete(recursive: true);
  });

  test('openBinary rejects recordId outside rootPath', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_bin_');
    final outside = await Directory.systemTemp.createTemp('sq_fallback_bin_o_');
    final file = File('${outside.path}${Platform.pathSeparator}sample.txt');
    await file.writeAsString('outside');

    await expectLater(
      fallback.openBinary(
        BinaryRequest(
          domain: QueryDomain.files,
          recordId: file.path,
          platformData: <String, Object?>{'rootPath': temp.path},
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
      ),
    );

    await temp.delete(recursive: true);
    await outside.delete(recursive: true);
  });

  group('cursor-based pagination via applyPaging', () {
    test('applyPaging with cursor skips records up to cursor id', () {
      final records = <QueryRecord>[
        {'id': 'a', 'name': 'first'},
        {'id': 'b', 'name': 'second'},
        {'id': 'c', 'name': 'third'},
        {'id': 'd', 'name': 'fourth'},
        {'id': 'e', 'name': 'fifth'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 2, cursor: 'b'),
      );

      expect(result.records, hasLength(2));
      expect(result.records[0]['id'], 'c');
      expect(result.records[1]['id'], 'd');
    });

    test('applyPaging with cursor returns nextCursor as last record id',
        () {
      final records = <QueryRecord>[
        {'id': 'a', 'name': 'first'},
        {'id': 'b', 'name': 'second'},
        {'id': 'c', 'name': 'third'},
        {'id': 'd', 'name': 'fourth'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 2, cursor: 'a'),
      );

      expect(result.records, hasLength(2));
      expect(result.records.last['id'], 'c');
      expect(result.nextCursor, 'c');
      expect(result.nextOffset, 3);
    });

    test('applyPaging with cursor at last element returns empty', () {
      final records = <QueryRecord>[
        {'id': 'a', 'name': 'first'},
        {'id': 'b', 'name': 'second'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 2, cursor: 'b'),
      );

      expect(result.records, isEmpty);
      expect(result.nextCursor, isNull);
      expect(result.nextOffset, isNull);
    });

    test('applyPaging with invalid cursor (not found) returns empty', () {
      final records = <QueryRecord>[
        {'id': 'a', 'name': 'first'},
        {'id': 'b', 'name': 'second'},
        {'id': 'c', 'name': 'third'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 10, cursor: 'nonexistent'),
      );

      expect(result.records, isEmpty);
      expect(result.nextCursor, isNull);
      expect(result.nextOffset, isNull);
    });

    test('applyPaging with null page returns all records', () {
      final records = <QueryRecord>[
        {'id': 'a'},
        {'id': 'b'},
        {'id': 'c'},
      ];

      final result = fallback.applyPaging(records, null);

      expect(result.records, hasLength(3));
      expect(result.nextOffset, isNull);
      expect(result.nextCursor, isNull);
    });

    test('applyPaging with cursor and no more records returns null nextCursor',
        () {
      final records = <QueryRecord>[
        {'id': 'a'},
        {'id': 'b'},
        {'id': 'c'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 10, cursor: 'a'),
      );

      expect(result.records, hasLength(2));
      expect(result.records[0]['id'], 'b');
      expect(result.records[1]['id'], 'c');
      expect(result.nextCursor, isNull);
      expect(result.nextOffset, isNull);
    });

    test('applyPaging with offset-based pagination still works', () {
      final records = <QueryRecord>[
        {'id': 'a'},
        {'id': 'b'},
        {'id': 'c'},
        {'id': 'd'},
      ];

      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 2, offset: 1),
      );

      expect(result.records, hasLength(2));
      expect(result.records[0]['id'], 'b');
      expect(result.records[1]['id'], 'c');
      expect(result.nextOffset, 3);
      expect(result.nextCursor, 'c');
    });

    test('applyPaging cursor takes precedence over offset', () {
      final records = <QueryRecord>[
        {'id': 'a'},
        {'id': 'b'},
        {'id': 'c'},
        {'id': 'd'},
        {'id': 'e'},
      ];

      // Cursor 'b' means start after 'b', so offset=0 should be ignored
      final result = fallback.applyPaging(
        records,
        const QueryPage(limit: 2, offset: 0, cursor: 'b'),
      );

      expect(result.records, hasLength(2));
      expect(result.records[0]['id'], 'c');
      expect(result.records[1]['id'], 'd');
    });
  });

  test('batch continues after filesystem exception', () async {
    final temp = await Directory.systemTemp.createTemp('sq_fallback_batch_io_');
    final doomed = File('${temp.path}${Platform.pathSeparator}doomed.txt');
    final survivor = File('${temp.path}${Platform.pathSeparator}survivor.txt');
    await doomed.writeAsString('before');
    await survivor.writeAsString('before');

    final result = await fallback.batch(
      BatchRequest(
        platformData: <String, Object?>{'rootPath': temp.path},
        operations: <MutationRequest>[
          MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.update,
            values: <String, Object?>{
              'path': doomed.path,
              'newPath': temp.path,
            },
          ),
          MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.update,
            values: <String, Object?>{
              'path': survivor.path,
              'content': 'after',
            },
          ),
        ],
      ),
    );

    expect(result.results, hasLength(2));
    expect(
      (result.results.first.metadata?['error']
          as Map<String, Object?>?)?['code'],
      'unavailable',
    );
    expect(await survivor.readAsString(), 'after');

    await temp.delete(recursive: true);
  });
}
