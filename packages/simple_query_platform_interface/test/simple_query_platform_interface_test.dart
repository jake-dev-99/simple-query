import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

void main() {
  final original = SimpleQueryPlatform.instance;

  tearDown(() {
    SimpleQueryPlatform.instance = original;
  });

  test('unsupported default exposes notSupported capabilities', () async {
    final snapshot = await SimpleQueryPlatform.instance.getCapabilities();
    expect(snapshot.capabilities.length, QueryDomain.values.length);
    expect(CapabilityContracts.isComplete(snapshot.capabilities), isTrue);
    expect(
        snapshot.capabilities.every((item) => item.canRead == false), isTrue);
  });

  test('unsupported default query throws SimpleQueryError.notSupported', () {
    expect(
      () => SimpleQueryPlatform.instance.query(
        const QueryRequest(domain: QueryDomain.contacts),
      ),
      throwsA(
        isA<SimpleQueryError>().having(
          (e) => e.code,
          'code',
          SimpleQueryErrorCode.notSupported,
        ),
      ),
    );
  });

  test('unsupported default observe stream emits deterministic error',
      () async {
    await expectLater(
      SimpleQueryPlatform.instance
          .observe(const ObserveRequest(domain: QueryDomain.files)),
      emitsError(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.notSupported)
            .having((e) => e.message, 'message', contains('simple_query:')),
      ),
    );
  });

  test('token verification accepts valid platform', () {
    final platform = _TestPlatform();
    SimpleQueryPlatform.instance = platform;
    expect(SimpleQueryPlatform.instance, same(platform));
  });

  test('domain contracts require expected keys for files', () {
    const valid = <String, Object?>{
      'id': '1',
      'path': '/tmp/a',
      'name': 'a',
      'isDirectory': false,
    };
    final missing = QueryDomainContracts.missingKeys(
      domain: QueryDomain.files,
      record: valid,
    );
    expect(missing, isEmpty);
  });

  test('domain contracts detect missing keys', () {
    const invalid = <String, Object?>{'id': '1', 'name': 'a'};
    final missing = QueryDomainContracts.missingKeys(
      domain: QueryDomain.files,
      record: invalid,
    );
    expect(missing, contains('path'));
    expect(missing, contains('isDirectory'));
  });

  test('domain contracts expose optional and unknown keys', () {
    const record = <String, Object?>{
      'id': '1',
      'path': '/tmp/a',
      'name': 'a',
      'isDirectory': false,
      'size': 10,
      'unexpected': true,
    };
    final unknown = QueryDomainContracts.unknownKeys(
      domain: QueryDomain.files,
      record: record,
    );
    expect(unknown, contains('unexpected'));
  });

  test('domain contract allowlists are deterministic for all core domains', () {
    for (final domain in <QueryDomain>[
      QueryDomain.contacts,
      QueryDomain.calendar,
      QueryDomain.media,
      QueryDomain.files,
      QueryDomain.messages,
      QueryDomain.calls,
    ]) {
      final allowed = QueryDomainContracts.allowedKeysFor(domain);
      expect(allowed, isNotEmpty, reason: 'allowed keys empty for $domain');
      final required = QueryDomainContracts.requiredKeys[domain]!;
      expect(allowed.containsAll(required), isTrue);
    }
  });

  test('capability contracts report missing domains', () {
    final partial = <CapabilityDescriptor>[
      const CapabilityDescriptor(
        domain: QueryDomain.files,
        canRead: true,
        canWrite: true,
        canObserve: true,
        canStream: true,
      ),
    ];
    final missing = CapabilityContracts.missingDomains(partial);
    expect(missing, contains(QueryDomain.contacts));
    expect(missing, contains(QueryDomain.calendar));
    expect(CapabilityContracts.isComplete(partial), isFalse);
  });

  test('runtime validation rejects incomplete capability snapshots', () {
    expect(
      () => RuntimeContractValidation.validateCapabilitySnapshot(
        const CapabilitySnapshot(
          capabilities: <CapabilityDescriptor>[
            CapabilityDescriptor(
              domain: QueryDomain.files,
              canRead: true,
              canWrite: true,
              canObserve: true,
              canStream: true,
            ),
          ],
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.unavailable),
      ),
    );
  });

  test('runtime validation rejects missing required query keys', () {
    expect(
      () => RuntimeContractValidation.validateQueryResult(
        domain: QueryDomain.files,
        result: const QueryResult(
          records: <QueryRecord>[
            <String, Object?>{'id': '1', 'name': 'file.txt'},
          ],
        ),
      ),
      throwsA(
        isA<SimpleQueryError>()
            .having((e) => e.code, 'code', SimpleQueryErrorCode.unavailable),
      ),
    );
  });

  test('runtime validation allows additive keys on core domains', () {
    final result = RuntimeContractValidation.validateQueryResult(
      domain: QueryDomain.files,
      result: const QueryResult(
        records: <QueryRecord>[
          <String, Object?>{
            'id': '1',
            'path': '/tmp/a',
            'name': 'a',
            'isDirectory': false,
            'unexpected': true,
          },
        ],
      ),
    );

    expect(result.records, hasLength(1));
  });

  group('model equality', () {
    test('two identical QueryRequest instances are equal', () {
      const a = QueryRequest(
        domain: QueryDomain.contacts,
        entityType: 'people',
        filters: [
          QueryFilterCondition(
            field: 'name',
            operator: QueryFilterOperator.equals,
            value: 'Alice',
          ),
        ],
        sort: [QuerySort(field: 'name')],
        page: QueryPage(limit: 10, offset: 0),
      );
      const b = QueryRequest(
        domain: QueryDomain.contacts,
        entityType: 'people',
        filters: [
          QueryFilterCondition(
            field: 'name',
            operator: QueryFilterOperator.equals,
            value: 'Alice',
          ),
        ],
        sort: [QuerySort(field: 'name')],
        page: QueryPage(limit: 10, offset: 0),
      );
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
    });

    test('different QueryRequest instances are not equal', () {
      const a = QueryRequest(domain: QueryDomain.contacts);
      const b = QueryRequest(domain: QueryDomain.files);
      expect(a, isNot(equals(b)));
    });

    test('QueryPage with cursor field equality', () {
      const a = QueryPage(limit: 20, cursor: 'abc123');
      const b = QueryPage(limit: 20, cursor: 'abc123');
      const c = QueryPage(limit: 20, cursor: 'def456');
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
      expect(a, isNot(equals(c)));
    });

    test('QueryResult with nextCursor field equality', () {
      const a = QueryResult(
        records: [
          <String, Object?>{'id': '1'}
        ],
        nextCursor: 'cursor1',
      );
      const b = QueryResult(
        records: [
          <String, Object?>{'id': '1'}
        ],
        nextCursor: 'cursor1',
      );
      const c = QueryResult(
        records: [
          <String, Object?>{'id': '1'}
        ],
        nextCursor: 'cursor2',
      );
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
      expect(a, isNot(equals(c)));
    });

    test('deep equality for filters list in QueryRequest', () {
      const a = QueryRequest(
        domain: QueryDomain.messages,
        filters: [
          QueryFilterCondition(
            field: 'type',
            operator: QueryFilterOperator.inList,
            value: [1, 2, 3],
          ),
        ],
      );
      const b = QueryRequest(
        domain: QueryDomain.messages,
        filters: [
          QueryFilterCondition(
            field: 'type',
            operator: QueryFilterOperator.inList,
            value: [1, 2, 3],
          ),
        ],
      );
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
    });

    test('deep equality for metadata map in QueryResult', () {
      const a = QueryResult(
        records: [],
        metadata: {'key': 'value', 'nested': true},
      );
      const b = QueryResult(
        records: [],
        metadata: {'key': 'value', 'nested': true},
      );
      expect(a, equals(b));
    });

    test('QueryFilterCondition equality', () {
      const a = QueryFilterCondition(
        field: 'name',
        operator: QueryFilterOperator.contains,
        value: 'test',
      );
      const b = QueryFilterCondition(
        field: 'name',
        operator: QueryFilterOperator.contains,
        value: 'test',
      );
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
    });

    test('QuerySort equality', () {
      const a = QuerySort(
        field: 'date',
        direction: QuerySortDirection.descending,
      );
      const b = QuerySort(
        field: 'date',
        direction: QuerySortDirection.descending,
      );
      expect(a, equals(b));
      expect(a.hashCode, equals(b.hashCode));
    });
  });

  group('model copyWith', () {
    test('QueryRequest copyWith preserves unmodified fields', () {
      const original = QueryRequest(
        domain: QueryDomain.contacts,
        entityType: 'people',
        filters: [
          QueryFilterCondition(
            field: 'name',
            operator: QueryFilterOperator.equals,
            value: 'Alice',
          ),
        ],
        page: QueryPage(limit: 10, offset: 0),
      );
      final modified = original.copyWith(domain: QueryDomain.files);
      expect(modified.domain, QueryDomain.files);
      expect(modified.entityType, 'people');
      expect(modified.filters, original.filters);
      expect(modified.page, original.page);
    });

    test('QueryRequest copyWith changes specified fields', () {
      const original = QueryRequest(domain: QueryDomain.contacts);
      final modified = original.copyWith(
        entityType: 'organizations',
        page: const QueryPage(limit: 50),
      );
      expect(modified.entityType, 'organizations');
      expect(modified.page!.limit, 50);
      expect(modified.domain, QueryDomain.contacts);
    });

    test('QueryPage copyWith preserves cursor', () {
      const original = QueryPage(limit: 10, cursor: 'abc');
      final modified = original.copyWith(limit: 20);
      expect(modified.limit, 20);
      expect(modified.cursor, 'abc');
    });

    test('QueryResult copyWith preserves nextCursor', () {
      const original = QueryResult(
        records: [
          <String, Object?>{'id': '1'}
        ],
        nextCursor: 'cur1',
        totalCount: 100,
      );
      final modified = original.copyWith(totalCount: 200);
      expect(modified.totalCount, 200);
      expect(modified.nextCursor, 'cur1');
      expect(modified.records, original.records);
    });

    test('QueryFilterCondition copyWith', () {
      const original = QueryFilterCondition(
        field: 'name',
        operator: QueryFilterOperator.equals,
        value: 'Alice',
      );
      final modified = original.copyWith(value: 'Bob');
      expect(modified.field, 'name');
      expect(modified.operator, QueryFilterOperator.equals);
      expect(modified.value, 'Bob');
    });

    test('QuerySort copyWith', () {
      const original = QuerySort(
        field: 'date',
        direction: QuerySortDirection.ascending,
      );
      final modified =
          original.copyWith(direction: QuerySortDirection.descending);
      expect(modified.field, 'date');
      expect(modified.direction, QuerySortDirection.descending);
    });

    test('MutationRequest copyWith', () {
      const original = MutationRequest(
        domain: QueryDomain.contacts,
        type: MutationType.insert,
        values: {'name': 'Alice'},
      );
      final modified = original.copyWith(type: MutationType.update);
      expect(modified.type, MutationType.update);
      expect(modified.domain, QueryDomain.contacts);
      expect(modified.values, {'name': 'Alice'});
    });

    test('CapabilityDescriptor copyWith', () {
      const original = CapabilityDescriptor(
        domain: QueryDomain.files,
        canRead: true,
        canWrite: true,
        canObserve: false,
        canStream: false,
      );
      final modified = original.copyWith(canObserve: true);
      expect(modified.canObserve, true);
      expect(modified.canRead, true);
      expect(modified.canWrite, true);
      expect(modified.canStream, false);
      expect(modified.domain, QueryDomain.files);
    });

    test('BatchRequest copyWith', () {
      const original = BatchRequest(
        operations: [
          MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.delete,
          ),
        ],
      );
      final modified = original.copyWith(
        platformData: {'key': 'val'},
      );
      expect(modified.operations, hasLength(1));
      expect(modified.platformData, {'key': 'val'});
    });
  });

  group('copyWith null clears nullable fields', () {
    test('QueryPage clears offset and cursor independently', () {
      const page = QueryPage(limit: 10, offset: 5, cursor: 'c');
      final clearedOffset = page.copyWith(offset: null);
      expect(clearedOffset.offset, isNull);
      expect(clearedOffset.cursor, 'c');
      expect(clearedOffset.limit, 10);

      final clearedCursor = page.copyWith(cursor: null);
      expect(clearedCursor.cursor, isNull);
      expect(clearedCursor.offset, 5);

      // Omitted params preserve existing values.
      final untouched = page.copyWith();
      expect(untouched, equals(page));
    });

    test('QueryResult clears nextOffset/nextCursor/metadata independently',
        () {
      const result = QueryResult(
        records: <QueryRecord>[],
        totalCount: 1,
        nextOffset: 10,
        nextCursor: 'k',
        metadata: <String, Object?>{'x': 1},
      );
      expect(result.copyWith(nextOffset: null).nextOffset, isNull);
      expect(result.copyWith(nextOffset: null).nextCursor, 'k');
      expect(result.copyWith(nextCursor: null).nextCursor, isNull);
      expect(result.copyWith(metadata: null).metadata, isNull);
    });

    test('QueryRequest clears projection/page/platformData independently', () {
      const request = QueryRequest(
        domain: QueryDomain.files,
        entityType: 'e',
        projection: <String>['a'],
        page: QueryPage(limit: 1),
        platformData: <String, Object?>{'k': 'v'},
      );
      expect(request.copyWith(entityType: null).entityType, isNull);
      expect(request.copyWith(projection: null).projection, isNull);
      expect(request.copyWith(page: null).page, isNull);
      expect(request.copyWith(platformData: null).platformData, isNull);
    });

    test('MutationRequest clears optional fields', () {
      const mutation = MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.insert,
        entityType: 'e',
        values: <String, Object?>{'k': 'v'},
        platformData: <String, Object?>{'p': 1},
      );
      expect(mutation.copyWith(entityType: null).entityType, isNull);
      expect(mutation.copyWith(values: null).values, isNull);
      expect(mutation.copyWith(platformData: null).platformData, isNull);
    });

    test('MutationResult clears optional fields', () {
      const result = MutationResult(
        affectedCount: 1,
        insertedId: 'id',
        metadata: <String, Object?>{'k': 'v'},
      );
      expect(result.copyWith(affectedCount: null).affectedCount, isNull);
      expect(result.copyWith(insertedId: null).insertedId, isNull);
      expect(result.copyWith(metadata: null).metadata, isNull);
    });

    test('ObserveRequest clears optional fields', () {
      const request = ObserveRequest(
        domain: QueryDomain.files,
        entityType: 'e',
        pollingInterval: Duration(seconds: 1),
        platformData: <String, Object?>{'k': 'v'},
      );
      expect(request.copyWith(entityType: null).entityType, isNull);
      expect(
        request.copyWith(pollingInterval: null).pollingInterval,
        isNull,
      );
      expect(request.copyWith(platformData: null).platformData, isNull);
    });

    test('ObserveEvent clears optional fields', () {
      final event = ObserveEvent(
        domain: QueryDomain.files,
        changeType: ObserveChangeType.update,
        timestamp: DateTime.utc(2024),
        entityType: 'e',
        source: 's',
        metadata: const <String, Object?>{'k': 'v'},
      );
      expect(event.copyWith(entityType: null).entityType, isNull);
      expect(event.copyWith(source: null).source, isNull);
      expect(event.copyWith(metadata: null).metadata, isNull);
    });

    test('BinaryRequest clears optional fields', () {
      const request = BinaryRequest(
        domain: QueryDomain.files,
        entityType: 'e',
        recordId: 'r',
        platformData: <String, Object?>{'k': 'v'},
      );
      expect(request.copyWith(entityType: null).entityType, isNull);
      expect(request.copyWith(recordId: null).recordId, isNull);
      expect(request.copyWith(platformData: null).platformData, isNull);
    });

    test('BinaryContentHandle clears optional fields', () {
      const handle = BinaryContentHandle(
        handleId: 'h',
        localPath: '/p',
        mimeType: 'image/jpeg',
        size: 10,
        metadata: <String, Object?>{'k': 'v'},
      );
      expect(handle.copyWith(mimeType: null).mimeType, isNull);
      expect(handle.copyWith(size: null).size, isNull);
      expect(handle.copyWith(metadata: null).metadata, isNull);
    });

    test('CapabilityDescriptor clears reason', () {
      const cap = CapabilityDescriptor(
        domain: QueryDomain.files,
        canRead: true,
        canWrite: false,
        canObserve: false,
        canStream: false,
        reason: 'because',
      );
      expect(cap.copyWith(reason: null).reason, isNull);
    });

    test('QueryFilterCondition clears value', () {
      const condition = QueryFilterCondition(
        field: 'f',
        operator: QueryFilterOperator.equals,
        value: 'v',
      );
      expect(condition.copyWith(value: null).value, isNull);
    });
  });

  test('runtime validation allows platformSpecific records', () {
    final result = RuntimeContractValidation.validateQueryResult(
      domain: QueryDomain.platformSpecific,
      result: const QueryResult(
        records: <QueryRecord>[
          <String, Object?>{'anything': true},
        ],
      ),
    );

    expect(result.records, hasLength(1));
  });
}

class _TestPlatform extends SimpleQueryPlatform {
  @override
  Future<BatchResult> batch(BatchRequest request) async =>
      const BatchResult(results: <MutationResult>[]);

  @override
  Future<void> closeBinary(String handleId) async {}

  @override
  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async =>
      null;

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
  Future<MutationResult> mutate(MutationRequest request) async =>
      const MutationResult(affectedCount: 0);

  @override
  Stream<ObserveEvent> observe(ObserveRequest request) => const Stream.empty();

  @override
  Future<BinaryContentHandle> openBinary(BinaryRequest request) async =>
      const BinaryContentHandle(handleId: 'h', localPath: '/tmp/h');

  @override
  Future<QueryResult> query(QueryRequest request) async =>
      const QueryResult(records: <QueryRecord>[]);
}
