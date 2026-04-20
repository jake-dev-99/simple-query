import 'package:flutter_test/flutter_test.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';
import 'package:simple_query/simple_query.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

void main() {
  final original = SimpleQueryPlatform.instance;

  tearDown(() {
    SimpleQueryPlatform.instance = original;
  });

  test('exposes singleton instance', () {
    expect(SimpleQuery.instance, isA<SimpleQuery>());
  });

  test('delegates query to platform', () async {
    final platform = _FakePlatform();
    SimpleQueryPlatform.instance = platform;

    final result = await SimpleQuery.instance.query(
      const QueryRequest(domain: QueryDomain.contacts),
    );

    expect(result.records.length, 1);
    expect(result.records.first['id'], 1);
  });

  test('supports capability lookup', () async {
    SimpleQueryPlatform.instance = _FakePlatform();

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
    SimpleQueryPlatform.instance = _FakePlatform();

    await SimpleQuery.instance.dispose();

    expect(SimpleQuery.instance, isA<SimpleQuery>());
  });

  test('delegates mutate, batch, binary, extension and observe', () async {
    final platform = _FakePlatform();
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
  });

  test('queryTyped maps records', () async {
    SimpleQueryPlatform.instance = _FakePlatform();

    final ids = await SimpleQuery.instance.queryTyped<int>(
      const QueryRequest(domain: QueryDomain.contacts),
      (record) => (record['id'] as int?) ?? 0,
    );

    expect(ids, <int>[1]);
  });

  test('queryTyped wraps fromRecord failures in SimpleQueryError', () async {
    SimpleQueryPlatform.instance = _FakePlatform();

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
    SimpleQueryPlatform.instance = _FakePlatform();

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

class _FakePlatform extends SimpleQueryPlatform
    with MockPlatformInterfaceMixin {
  int mutateCalls = 0;
  int batchCalls = 0;
  int openBinaryCalls = 0;
  int closeBinaryCalls = 0;
  int callExtensionCalls = 0;
  int observeCalls = 0;

  @override
  Future<BatchResult> batch(BatchRequest request) async {
    batchCalls += 1;
    return const BatchResult(
      results: <MutationResult>[MutationResult(affectedCount: 1)],
    );
  }

  @override
  Future<void> closeBinary(String handleId) async {
    closeBinaryCalls += 1;
  }

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    callExtensionCalls += 1;
    return <String, Object?>{'ok': true};
  }

  @override
  Future<void> dispose() async {}

  @override
  Future<CapabilitySnapshot> getCapabilities() async => CapabilitySnapshot(
        capabilities: QueryDomain.values
            .map(
              (domain) => CapabilityDescriptor(
                domain: domain,
                canRead: true,
                canWrite: true,
                canObserve: true,
                canStream: true,
              ),
            )
            .toList(growable: false),
      );

  @override
  Future<MutationResult> mutate(MutationRequest request) async {
    mutateCalls += 1;
    return const MutationResult(affectedCount: 1);
  }

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) =>
      Stream<ObserveEvent>.fromIterable(<ObserveEvent>[
        ObserveEvent(
          domain: request.domain,
          changeType: ObserveChangeType.unknown,
          timestamp: DateTime.fromMillisecondsSinceEpoch(0, isUtc: true),
        ),
      ]).map((event) {
        observeCalls += 1;
        return event;
      });

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) async {
    openBinaryCalls += 1;
    return const BinaryContentHandle(handleId: 'h1', localPath: '/tmp/h1');
  }

  @override
  Future<QueryResult> query(QueryRequest request) async => const QueryResult(
        records: <QueryRecord>[
          <String, Object?>{'id': 1}
        ],
      );
}
