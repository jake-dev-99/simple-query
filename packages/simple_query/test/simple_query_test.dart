import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query/simple_query.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_platform_interface/testing.dart';

void main() {
  final original = SimpleQueryPlatform.instance;

  tearDown(() {
    SimpleQueryPlatform.instance = original;
  });

  FakeSimpleQueryPlatform makeFake() => FakeSimpleQueryPlatform(
        queryResult: const QueryResult(
          records: <QueryRecord>[<String, Object?>{'id': 1}],
        ),
        mutationResult: const MutationResult(affectedCount: 1),
        batchResult: const BatchResult(
          results: <MutationResult>[MutationResult(affectedCount: 1)],
        ),
        binaryContentHandle: const BinaryContentHandle(
          handleId: 'h1',
          localPath: '/tmp/h1',
        ),
        observeStream: Stream<ObserveEvent>.fromIterable(<ObserveEvent>[
          ObserveEvent(
            domain: QueryDomain.files,
            changeType: ObserveChangeType.unknown,
            timestamp: DateTime.fromMillisecondsSinceEpoch(0, isUtc: true),
          ),
        ]),
        extensionResult: const <String, Object?>{'ok': true},
      );

  test('exposes singleton instance', () {
    expect(SimpleQuery.instance, isA<SimpleQuery>());
  });

  test('delegates query to platform', () async {
    final platform = makeFake();
    SimpleQueryPlatform.instance = platform;

    final result = await SimpleQuery.instance.query(
      const QueryRequest(domain: QueryDomain.contacts),
    );

    expect(result.records.length, 1);
    expect(result.records.first['id'], 1);
  });

  test('supports capability lookup', () async {
    SimpleQueryPlatform.instance = makeFake();

    final capabilities = await SimpleQuery.instance.getCapabilities();

    expect(capabilities.capabilities, isNotEmpty);
    expect(
      capabilities.capabilities
          .firstWhere((item) => item.domain == QueryDomain.contacts)
          .canRead,
      isTrue,
    );
  });

  test('dispose delegates to platform', () async {
    SimpleQueryPlatform.instance = makeFake();

    await SimpleQuery.instance.dispose();

    expect(SimpleQuery.instance, isA<SimpleQuery>());
  });

  test('delegates mutate, batch, binary, extension and observe', () async {
    final platform = makeFake();
    SimpleQueryPlatform.instance = platform;

    final mutation = await SimpleQuery.instance.mutate(
      const MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.insert,
        values: <String, Object?>{'path': '/tmp/x'},
      ),
    );
    expect(mutation.affectedCount, 1);

    final batch = await SimpleQuery.instance.batch(
      const BatchRequest(
        operations: <MutationRequest>[
          MutationRequest(domain: QueryDomain.files, type: MutationType.delete),
        ],
      ),
    );
    expect(batch.results, hasLength(1));

    final handle = await SimpleQuery.instance.openBinary(
      const BinaryRequest(domain: QueryDomain.files, recordId: '/tmp/x'),
    );
    expect(handle.handleId, 'h1');
    await SimpleQuery.instance.closeBinary(handle.handleId);

    final extension = await SimpleQuery.instance.callExtension(
      namespace: 'windows.storage',
      method: 'resolveKnownFolders',
    );
    expect(extension, isNotNull);

    final observed = await SimpleQuery.instance
        .observe(const ObserveRequest(domain: QueryDomain.files))
        .first;
    expect(observed.domain, QueryDomain.files);

    expect(platform.mutateCalls, 1);
    expect(platform.batchCalls, 1);
    expect(platform.openBinaryCalls, 1);
    expect(platform.closeBinaryCalls, 1);
    expect(platform.callExtensionCalls, 1);
    expect(platform.observeCalls, 1);
  });

  group('QueryBuilder', () {
    test('builder chains produce correct QueryRequest', () {
      final request = QueryBuilder(QueryDomain.contacts)
          .entityType('people')
          .where('name', QueryFilterOperator.equals, 'Alice')
          .select(['id', 'name', 'email'])
          .orderBy('name')
          .page(limit: 20, offset: 0)
          .build();

      expect(request.domain, QueryDomain.contacts);
      expect(request.entityType, 'people');
      expect(request.filters, hasLength(1));
      expect(request.filters.first.field, 'name');
      expect(request.filters.first.operator, QueryFilterOperator.equals);
      expect(request.filters.first.value, 'Alice');
      expect(request.projection, ['id', 'name', 'email']);
      expect(request.sort, hasLength(1));
      expect(request.sort.first.field, 'name');
      expect(request.sort.first.direction, QuerySortDirection.ascending);
      expect(request.page!.limit, 20);
      expect(request.page!.offset, 0);
    });

    test('page() with cursor parameter', () {
      final request = QueryBuilder(QueryDomain.messages)
          .page(limit: 50, cursor: 'abc123')
          .build();

      expect(request.page!.limit, 50);
      expect(request.page!.cursor, 'abc123');
      expect(request.page!.offset, isNull);
    });

    test('where() adds multiple filters correctly', () {
      final request = QueryBuilder(QueryDomain.messages)
          .where('type', QueryFilterOperator.equals, 1)
          .where('date', QueryFilterOperator.greaterThan, 1000)
          .where('address', QueryFilterOperator.contains, '555')
          .build();

      expect(request.filters, hasLength(3));
      expect(request.filters[0].field, 'type');
      expect(request.filters[0].operator, QueryFilterOperator.equals);
      expect(request.filters[1].field, 'date');
      expect(request.filters[1].operator, QueryFilterOperator.greaterThan);
      expect(request.filters[2].field, 'address');
      expect(request.filters[2].operator, QueryFilterOperator.contains);
    });

    test('orderBy() adds multiple sort rules', () {
      final request = QueryBuilder(QueryDomain.files)
          .orderBy('name', direction: QuerySortDirection.ascending)
          .orderBy('size', direction: QuerySortDirection.descending)
          .build();

      expect(request.sort, hasLength(2));
      expect(request.sort[0].field, 'name');
      expect(request.sort[0].direction, QuerySortDirection.ascending);
      expect(request.sort[1].field, 'size');
      expect(request.sort[1].direction, QuerySortDirection.descending);
    });

    test('build() returns immutable request', () {
      final request = QueryBuilder(QueryDomain.contacts)
          .where('name', QueryFilterOperator.equals, 'test')
          .orderBy('name')
          .platformData({'key': 'value'}).build();

      // Unmodifiable lists/maps throw when mutated.
      expect(
        () => (request.filters as List).add(
          const QueryFilterCondition(
            field: 'x',
            operator: QueryFilterOperator.equals,
          ),
        ),
        throwsA(isA<Error>()),
      );
      expect(
        () => (request.sort as List).add(
          const QuerySort(field: 'x'),
        ),
        throwsA(isA<Error>()),
      );
      if (request.platformData != null) {
        expect(
          () => (request.platformData as Map)['new'] = 'value',
          throwsA(isA<Error>()),
        );
      }
    });

    test('build() with no optional fields produces minimal request', () {
      final request = QueryBuilder(QueryDomain.calendar).build();

      expect(request.domain, QueryDomain.calendar);
      expect(request.entityType, isNull);
      expect(request.filters, isEmpty);
      expect(request.projection, isNull);
      expect(request.sort, isEmpty);
      expect(request.page, isNull);
      expect(request.platformData, isNull);
    });

    test('platformData() sets platform data', () {
      final request = QueryBuilder(QueryDomain.files)
          .platformData({'rootPath': '/tmp'}).build();

      expect(request.platformData, {'rootPath': '/tmp'});
    });

    test('execute() forwards the built request to the platform', () async {
      final fake = makeFake();
      SimpleQueryPlatform.instance = fake;

      final result = await QueryBuilder(QueryDomain.contacts)
          .entityType('people')
          .where('name', QueryFilterOperator.equals, 'Alice')
          .orderBy('name')
          .page(limit: 5)
          .execute();

      expect(result.records, hasLength(1));
      expect(fake.queryCalls, 1);
      final captured = fake.queryRequests.single;
      expect(captured.domain, QueryDomain.contacts);
      expect(captured.entityType, 'people');
      expect(captured.filters.single.field, 'name');
      expect(captured.sort.single.field, 'name');
      expect(captured.page?.limit, 5);
    });

    test('executeTyped() maps records and surfaces mapping failures', () async {
      SimpleQueryPlatform.instance = makeFake();

      final ids = await QueryBuilder(QueryDomain.contacts)
          .executeTyped<int>((record) => (record['id'] as int?) ?? -1);
      expect(ids, <int>[1]);

      await expectLater(
        () => QueryBuilder(QueryDomain.contacts).executeTyped<String>(
          (record) => throw StateError('boom'),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });
  });

  group('queryPaginated', () {
    test('walks cursor-based pagination to exhaustion', () async {
      final fake = makeFake()
        ..queryResultsByCall = const <QueryResult>[
          QueryResult(
            records: <QueryRecord>[
              <String, Object?>{'id': 1},
              <String, Object?>{'id': 2},
            ],
            nextCursor: 'a',
          ),
          QueryResult(
            records: <QueryRecord>[
              <String, Object?>{'id': 3},
            ],
            nextCursor: 'b',
          ),
          QueryResult(
            records: <QueryRecord>[
              <String, Object?>{'id': 4},
            ],
          ),
        ];
      SimpleQueryPlatform.instance = fake;

      final pages = await SimpleQuery.instance
          .queryPaginated(const QueryRequest(domain: QueryDomain.contacts))
          .toList();

      expect(pages, hasLength(3));
      expect(pages[0].records.length, 2);
      expect(pages[2].nextCursor, isNull);
      expect(fake.queryCalls, 3);

      // Subsequent requests carry the cursor from the previous result.
      expect(fake.queryRequests[1].page?.cursor, 'a');
      expect(fake.queryRequests[2].page?.cursor, 'b');
      expect(fake.queryRequests[1].page?.offset, isNull);
    });

    test('walks offset-based pagination to exhaustion', () async {
      final fake = makeFake()
        ..queryResultsByCall = const <QueryResult>[
          QueryResult(
            records: <QueryRecord>[<String, Object?>{'id': 1}],
            nextOffset: 1,
          ),
          QueryResult(
            records: <QueryRecord>[<String, Object?>{'id': 2}],
            nextOffset: 2,
          ),
          QueryResult(records: <QueryRecord>[<String, Object?>{'id': 3}]),
        ];
      SimpleQueryPlatform.instance = fake;

      final pages = await SimpleQuery.instance
          .queryPaginated(const QueryRequest(
            domain: QueryDomain.files,
            page: QueryPage(limit: 1),
          ))
          .toList();

      expect(pages, hasLength(3));
      expect(fake.queryRequests[1].page?.offset, 1);
      expect(fake.queryRequests[1].page?.limit, 1);
      expect(fake.queryRequests[2].page?.offset, 2);
    });

    test('cursor wins when both nextCursor and nextOffset are present',
        () async {
      final fake = makeFake()
        ..queryResultsByCall = const <QueryResult>[
          QueryResult(
            records: <QueryRecord>[<String, Object?>{'id': 1}],
            nextCursor: 'c',
            nextOffset: 99,
          ),
          QueryResult(records: <QueryRecord>[<String, Object?>{'id': 2}]),
        ];
      SimpleQueryPlatform.instance = fake;

      await SimpleQuery.instance
          .queryPaginated(const QueryRequest(domain: QueryDomain.files))
          .toList();

      expect(fake.queryRequests[1].page?.cursor, 'c');
      expect(fake.queryRequests[1].page?.offset, isNull);
    });

    test('stops on empty page even if pagination tokens are non-null',
        () async {
      final fake = makeFake()
        ..queryResultsByCall = const <QueryResult>[
          QueryResult(records: <QueryRecord>[], nextCursor: 'should-not-use'),
        ];
      SimpleQueryPlatform.instance = fake;

      final pages = await SimpleQuery.instance
          .queryPaginated(const QueryRequest(domain: QueryDomain.files))
          .toList();

      expect(pages, hasLength(1));
      expect(fake.queryCalls, 1);
    });

    test('queryPaginatedTyped maps each page', () async {
      final fake = makeFake()
        ..queryResultsByCall = const <QueryResult>[
          QueryResult(
            records: <QueryRecord>[
              <String, Object?>{'id': 1},
              <String, Object?>{'id': 2},
            ],
            nextCursor: 'a',
          ),
          QueryResult(
            records: <QueryRecord>[<String, Object?>{'id': 3}],
          ),
        ];
      SimpleQueryPlatform.instance = fake;

      final batches = await SimpleQuery.instance
          .queryPaginatedTyped<int>(
            const QueryRequest(domain: QueryDomain.contacts),
            (record) => (record['id'] as int?) ?? -1,
          )
          .toList();

      expect(batches, <List<int>>[
        <int>[1, 2],
        <int>[3],
      ]);
    });

    test('queryPaginatedTyped wraps mapping failures', () async {
      SimpleQueryPlatform.instance = makeFake();

      await expectLater(
        SimpleQuery.instance.queryPaginatedTyped<String>(
          const QueryRequest(domain: QueryDomain.contacts),
          (record) => throw StateError('boom'),
        ),
        emitsError(
          isA<SimpleQueryError>().having(
            (e) => e.code,
            'code',
            SimpleQueryErrorCode.invalidQuery,
          ),
        ),
      );
    });
  });

  test('queryRaw builds a platformSpecific request with the contentUri',
      () async {
    final fake = makeFake();
    SimpleQueryPlatform.instance = fake;

    await SimpleQuery.instance.queryRaw(
      contentUri: 'content://com.biz.app/data',
      filters: <QueryFilterCondition>[
        QueryFilterCondition(
          field: 'my_native_column',
          operator: QueryFilterOperator.equals,
          value: 'value',
        ),
      ],
      projection: const <String>['col_a', 'col_b'],
      platformData: const <String, Object?>{'extra': 1},
    );

    expect(fake.queryCalls, 1);
    final captured = fake.queryRequests.single;
    expect(captured.domain, QueryDomain.platformSpecific);
    expect(captured.platformData?['contentUri'], 'content://com.biz.app/data');
    expect(captured.platformData?['extra'], 1);
    expect(captured.filters.single.field, 'my_native_column');
    expect(captured.projection, <String>['col_a', 'col_b']);
  });

  test('queryTyped maps records', () async {
    SimpleQueryPlatform.instance = makeFake();

    final ids = await SimpleQuery.instance.queryTyped<int>(
      const QueryRequest(domain: QueryDomain.contacts),
      (record) => (record['id'] as int?) ?? 0,
    );

    expect(ids, <int>[1]);
  });

  test('queryTyped wraps fromRecord failures in SimpleQueryError', () async {
    SimpleQueryPlatform.instance = makeFake();

    await expectLater(
      () => SimpleQuery.instance.queryTyped<String>(
        const QueryRequest(domain: QueryDomain.contacts),
        (record) =>
            throw StateError('synthetic mapping failure'),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
            .having((e) => e.details?['recordIndex'], 'recordIndex', 0)
            .having((e) => e.details?['domain'], 'domain', 'contacts')
            .having(
              (e) => e.details?['cause'],
              'cause',
              contains('synthetic mapping failure'),
            ),
      ),
    );
  });

  test('queryTyped passes SimpleQueryError from fromRecord through unwrapped',
      () async {
    SimpleQueryPlatform.instance = makeFake();

    await expectLater(
      () => SimpleQuery.instance.queryTyped<String>(
        const QueryRequest(domain: QueryDomain.contacts),
        (record) => throw const SimpleQueryError(
          code: SimpleQueryErrorCode.permissionDenied,
          message: 'nope',
        ),
      ),
      throwsA(
        isA<SimpleQueryError>().having(
          (e) => e.code,
          'code',
          SimpleQueryErrorCode.permissionDenied,
        ),
      ),
    );
  });
}

