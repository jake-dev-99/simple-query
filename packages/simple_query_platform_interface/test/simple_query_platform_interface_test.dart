import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';
import 'package:simple_query_platform_interface/testing.dart';

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
    final platform = FakeSimpleQueryPlatform();
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
    test('QueryPage offset-mode clears offset, cursor-mode clears cursor', () {
      const offsetPage = QueryPage.offset(limit: 10, offset: 5);
      final clearedOffset = offsetPage.copyWith(offset: null);
      expect(clearedOffset.offset, isNull);
      expect(clearedOffset.limit, 10);

      const cursorPage = QueryPage.cursor(limit: 10, cursor: 'c');
      final clearedCursor = cursorPage.copyWith(cursor: null);
      expect(clearedCursor.cursor, isNull);
      expect(clearedCursor.limit, 10);

      // Omitted params preserve existing values.
      expect(offsetPage.copyWith(), equals(offsetPage));
      expect(cursorPage.copyWith(), equals(cursorPage));
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

  group('QueryFieldCatalog', () {
    test('canonicalFields returns required ∪ optional for each domain', () {
      for (final domain in QueryDomain.values) {
        if (domain == QueryDomain.platformSpecific) continue;
        final canonical = QueryFieldCatalog.canonicalFields(domain);
        final allowed = QueryDomainContracts.allowedKeysFor(domain);
        expect(canonical, allowed, reason: 'mismatch for ${domain.name}');
      }
    });

    test('canonicalFields is empty for platformSpecific', () {
      expect(
        QueryFieldCatalog.canonicalFields(QueryDomain.platformSpecific),
        isEmpty,
      );
    });

    test('ensureKnown passes for valid canonical field', () {
      QueryFieldCatalog.ensureKnown(
        domain: QueryDomain.calls,
        canonical: 'callType',
      );
      // No throw.
    });

    test('ensureKnown throws invalidQuery for unknown canonical field', () {
      expect(
        () => QueryFieldCatalog.ensureKnown(
          domain: QueryDomain.calls,
          canonical: 'type', // raw Android column name, not canonical
        ),
        throwsA(
          isA<SimpleQueryError>()
              .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
              .having(
                (e) => e.details?['field'],
                'details.field',
                'type',
              )
              .having(
                (e) => e.details?['domain'],
                'details.domain',
                'calls',
              ),
        ),
      );
    });

    test('ensureKnown is a no-op for platformSpecific', () {
      QueryFieldCatalog.ensureKnown(
        domain: QueryDomain.platformSpecific,
        canonical: 'whatever_you_want',
      );
      // No throw.
    });

    test('ensureAllKnown throws on the first unknown field', () {
      expect(
        () => QueryFieldCatalog.ensureAllKnown(
          domain: QueryDomain.files,
          fields: const <String>['path', 'unknown_field', 'name'],
        ),
        throwsA(
          isA<SimpleQueryError>().having(
            (e) => e.details?['field'],
            'details.field',
            'unknown_field',
          ),
        ),
      );
    });

    test('calls domain gains isNew/isRead/geocodedLocation/subscriptionId', () {
      final canonical = QueryFieldCatalog.canonicalFields(QueryDomain.calls);
      expect(canonical, containsAll(<String>[
        'isNew',
        'isRead',
        'geocodedLocation',
        'subscriptionId',
      ]));
    });

    test('error message lists allowed fields for the domain', () {
      try {
        QueryFieldCatalog.ensureKnown(
          domain: QueryDomain.files,
          canonical: 'not_a_real_field',
        );
        fail('expected throw');
      } on SimpleQueryError catch (e) {
        expect(e.message, contains('files'));
        expect(e.message, contains('not_a_real_field'));
        expect(e.details?['allowed'], isA<List<String>>());
      }
    });
  });

  group('model equality (full coverage)', () {
    test('MutationRequest equality and hashCode', () {
      const a = MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.insert,
        values: <String, Object?>{'k': 'v'},
      );
      const b = MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.insert,
        values: <String, Object?>{'k': 'v'},
      );
      const c = MutationRequest(
        domain: QueryDomain.files,
        type: MutationType.delete,
      );
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('MutationResult equality and hashCode', () {
      const a = MutationResult(affectedCount: 1, insertedId: 'x');
      const b = MutationResult(affectedCount: 1, insertedId: 'x');
      const c = MutationResult(affectedCount: 2);
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('BatchRequest equality and hashCode', () {
      const op =
          MutationRequest(domain: QueryDomain.files, type: MutationType.delete);
      const a = BatchRequest(operations: <MutationRequest>[op]);
      const b = BatchRequest(operations: <MutationRequest>[op]);
      const c = BatchRequest(operations: <MutationRequest>[op, op]);
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('BatchResult equality and hashCode', () {
      const a = BatchResult(
        results: <MutationResult>[MutationResult(affectedCount: 1)],
      );
      const b = BatchResult(
        results: <MutationResult>[MutationResult(affectedCount: 1)],
      );
      const c = BatchResult(
        results: <MutationResult>[MutationResult(affectedCount: 2)],
      );
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('ObserveRequest equality and hashCode', () {
      const a = ObserveRequest(
        domain: QueryDomain.files,
        pollingInterval: Duration(seconds: 1),
      );
      const b = ObserveRequest(
        domain: QueryDomain.files,
        pollingInterval: Duration(seconds: 1),
      );
      const c = ObserveRequest(domain: QueryDomain.calendar);
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('ObserveEvent equality and hashCode', () {
      final timestamp = DateTime.utc(2024);
      final a = ObserveEvent(
        domain: QueryDomain.files,
        changeType: ObserveChangeType.update,
        timestamp: timestamp,
        ids: const <String>['1', '2'],
      );
      final b = ObserveEvent(
        domain: QueryDomain.files,
        changeType: ObserveChangeType.update,
        timestamp: timestamp,
        ids: const <String>['1', '2'],
      );
      final c = ObserveEvent(
        domain: QueryDomain.files,
        changeType: ObserveChangeType.insert,
        timestamp: timestamp,
      );
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('BinaryRequest equality and hashCode', () {
      const a = BinaryRequest(domain: QueryDomain.files, recordId: 'r');
      const b = BinaryRequest(domain: QueryDomain.files, recordId: 'r');
      const c = BinaryRequest(domain: QueryDomain.media, recordId: 'r');
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });

    test('BinaryContentHandle equality and hashCode', () {
      const a = BinaryContentHandle(
        handleId: 'h',
        localPath: '/tmp/h',
        size: 100,
      );
      const b = BinaryContentHandle(
        handleId: 'h',
        localPath: '/tmp/h',
        size: 100,
      );
      const c = BinaryContentHandle(handleId: 'h2', localPath: '/tmp/h');
      expect(a, equals(b));
      expect(a.hashCode, b.hashCode);
      expect(a, isNot(equals(c)));
    });
  });

  group('domain contract ↔ record alignment', () {
    // Every required key in QueryDomainContracts must be read by the
    // corresponding *Record.fromRecord. Prevents drift between the contract
    // and the Dart typed view.
    test('ContactRecord covers all required contacts keys', () {
      const record = <String, Object?>{
        'id': '1',
        'displayName': 'n',
        'phones': <String>[],
        'emails': <String>[],
      };
      final parsed = ContactRecord.fromRecord(record);
      expect(parsed.id, '1');
      expect(parsed.displayName, 'n');
      for (final key in QueryDomainContracts.requiredKeys[QueryDomain.contacts]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });

    test('CalendarEventRecord covers all required calendar keys', () {
      const record = <String, Object?>{
        'id': '1',
        'title': 't',
        'startAt': '0',
        'endAt': '0',
        'isAllDay': false,
        'calendarId': 'c',
      };
      CalendarEventRecord.fromRecord(record);
      for (final key
          in QueryDomainContracts.requiredKeys[QueryDomain.calendar]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });

    test('MediaRecord covers all required media keys', () {
      const record = <String, Object?>{
        'id': '1',
        'uriOrPath': '/p',
        'mediaType': 'image',
      };
      MediaRecord.fromRecord(record);
      for (final key in QueryDomainContracts.requiredKeys[QueryDomain.media]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });

    test('FileRecord covers all required files keys', () {
      const record = <String, Object?>{
        'id': '1',
        'path': '/p',
        'name': 'n',
        'isDirectory': false,
      };
      FileRecord.fromRecord(record);
      for (final key in QueryDomainContracts.requiredKeys[QueryDomain.files]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });

    test('MessageRecord covers all required messages keys', () {
      const record = <String, Object?>{'id': '1', 'timestamp': '0'};
      MessageRecord.fromRecord(record);
      for (final key
          in QueryDomainContracts.requiredKeys[QueryDomain.messages]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });

    test('CallRecord covers all required calls keys', () {
      const record = <String, Object?>{
        'id': '1',
        'callType': 'incoming',
        'timestamp': '0',
      };
      CallRecord.fromRecord(record);
      for (final key in QueryDomainContracts.requiredKeys[QueryDomain.calls]!) {
        expect(record.containsKey(key), isTrue, reason: 'missing $key');
      }
    });
  });

  group('Constructor guards', () {
    test('QueryPage rejects simultaneous offset and cursor', () {
      expect(
        () => QueryPage(limit: 10, offset: 0, cursor: 'abc'),
        throwsA(isA<AssertionError>()),
      );
    });

    test('QueryPage.offset builds an offset-mode page', () {
      const page = QueryPage.offset(limit: 10, offset: 20);
      expect(page.offset, 20);
      expect(page.cursor, isNull);
    });

    test('QueryPage.cursor builds a cursor-mode page', () {
      const page = QueryPage.cursor(limit: 10, cursor: 'abc');
      expect(page.cursor, 'abc');
      expect(page.offset, isNull);
    });

    test('QueryFilterCondition asserts inList value is a List', () {
      expect(
        () => QueryFilterCondition(
          field: 'id',
          operator: QueryFilterOperator.inList,
          value: 'not-a-list',
        ),
        throwsA(isA<AssertionError>()),
      );
      // Valid: value is a List.
      const valid = QueryFilterCondition(
        field: 'id',
        operator: QueryFilterOperator.inList,
        value: <String>['1', '2'],
      );
      expect(valid.value, <String>['1', '2']);
    });
  });

  group('SimpleQueryError.toString', () {
    test('includes code, message, domain, operation, and details', () {
      const error = SimpleQueryError(
        code: SimpleQueryErrorCode.invalidQuery,
        message: 'bad request',
        domain: QueryDomain.files,
        operation: QueryOperation.read,
        details: <String, Object?>{'recordIndex': 3, 'field': 'path'},
      );
      final text = error.toString();
      expect(text, contains('invalidQuery'));
      expect(text, contains('bad request'));
      expect(text, contains('domain=files'));
      expect(text, contains('operation=read'));
      expect(text, contains('recordIndex'));
      expect(text, contains('path'));
    });

    test('omits details when absent', () {
      const error = SimpleQueryError(
        code: SimpleQueryErrorCode.notSupported,
        message: 'nope',
      );
      expect(error.toString(), isNot(contains('details=')));
    });
  });

  group('RuntimeContractValidation extra validators', () {
    test('validateMutationRequest rejects empty insert values', () {
      expect(
        () => RuntimeContractValidation.validateMutationRequest(
          const MutationRequest(
            domain: QueryDomain.files,
            type: MutationType.insert,
          ),
        ),
        throwsA(
          isA<SimpleQueryError>()
              .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
              .having((e) => e.details?['type'], 'details.type', 'insert'),
        ),
      );
    });

    test('validateMutationRequest accepts delete without values', () {
      final request = RuntimeContractValidation.validateMutationRequest(
        const MutationRequest(
          domain: QueryDomain.files,
          type: MutationType.delete,
        ),
      );
      expect(request.type, MutationType.delete);
    });

    test('validateQueryRequest rejects unknown filter field', () {
      expect(
        () => RuntimeContractValidation.validateQueryRequest(
          const QueryRequest(
            domain: QueryDomain.calls,
            filters: <QueryFilterCondition>[
              QueryFilterCondition(
                field: 'type', // raw Android column, not canonical
                operator: QueryFilterOperator.equals,
                value: '1',
              ),
            ],
          ),
        ),
        throwsA(
          isA<SimpleQueryError>()
              .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
              .having((e) => e.details?['field'], 'details.field', 'type'),
        ),
      );
    });

    test('validateQueryRequest rejects unknown sort field', () {
      expect(
        () => RuntimeContractValidation.validateQueryRequest(
          const QueryRequest(
            domain: QueryDomain.calls,
            sort: <QuerySort>[QuerySort(field: 'date')],
          ),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('validateQueryRequest rejects unknown projection field', () {
      expect(
        () => RuntimeContractValidation.validateQueryRequest(
          const QueryRequest(
            domain: QueryDomain.calls,
            projection: <String>['_id'],
          ),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('validateQueryRequest passes for canonical fields', () {
      RuntimeContractValidation.validateQueryRequest(
        const QueryRequest(
          domain: QueryDomain.calls,
          filters: <QueryFilterCondition>[
            QueryFilterCondition(
              field: 'callType',
              operator: QueryFilterOperator.equals,
              value: '1',
            ),
          ],
          sort: <QuerySort>[QuerySort(field: 'timestamp')],
          projection: <String>['id', 'number', 'durationSec'],
        ),
      );
      // No throw.
    });

    test('validateQueryRequest is a no-op for platformSpecific', () {
      RuntimeContractValidation.validateQueryRequest(
        const QueryRequest(
          domain: QueryDomain.platformSpecific,
          filters: <QueryFilterCondition>[
            QueryFilterCondition(
              field: 'anything_goes',
              operator: QueryFilterOperator.equals,
              value: 'x',
            ),
          ],
        ),
      );
      // No throw.
    });

    test('validateMutationRequest rejects unknown filter field', () {
      expect(
        () => RuntimeContractValidation.validateMutationRequest(
          const MutationRequest(
            domain: QueryDomain.calls,
            type: MutationType.delete,
            filters: <QueryFilterCondition>[
              QueryFilterCondition(
                field: 'date',
                operator: QueryFilterOperator.equals,
                value: '0',
              ),
            ],
          ),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('validateObserveRequest rejects unknown filter field', () {
      expect(
        () => RuntimeContractValidation.validateObserveRequest(
          const ObserveRequest(
            domain: QueryDomain.calls,
            filters: <QueryFilterCondition>[
              QueryFilterCondition(
                field: 'date',
                operator: QueryFilterOperator.equals,
                value: '0',
              ),
            ],
          ),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('validateBatchRequest rejects empty operations', () {
      expect(
        () => RuntimeContractValidation.validateBatchRequest(
          const BatchRequest(operations: <MutationRequest>[]),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test(
        'validateBatchRequest does NOT cascade per-op validation '
        '(sequentialBestEffort)', () {
      // A malformed operation (insert without values) must not abort the
      // whole batch at validation time — the batch runner records the
      // per-op failure in metadata.error and continues with the rest.
      final validated = RuntimeContractValidation.validateBatchRequest(
        const BatchRequest(
          operations: <MutationRequest>[
            MutationRequest(
              domain: QueryDomain.files,
              type: MutationType.update,
              // no `values` — would fail validateMutationRequest on its own
            ),
          ],
        ),
      );
      expect(validated.operations, hasLength(1));
    });

    test('validateObserveRequest rejects non-positive pollingInterval', () {
      expect(
        () => RuntimeContractValidation.validateObserveRequest(
          const ObserveRequest(
            domain: QueryDomain.files,
            pollingInterval: Duration.zero,
          ),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('validateBinaryRequest requires recordId or platformData', () {
      expect(
        () => RuntimeContractValidation.validateBinaryRequest(
          const BinaryRequest(domain: QueryDomain.files),
        ),
        throwsA(isA<SimpleQueryError>()),
      );
      // recordId alone is sufficient.
      RuntimeContractValidation.validateBinaryRequest(
        const BinaryRequest(domain: QueryDomain.files, recordId: 'r'),
      );
    });

    test('validateQueryResult includes domain in details', () {
      try {
        RuntimeContractValidation.validateQueryResult(
          domain: QueryDomain.files,
          result: const QueryResult(
            records: <QueryRecord>[
              <String, Object?>{'id': '1'}, // missing required keys
            ],
          ),
        );
        fail('expected throw');
      } on SimpleQueryError catch (error) {
        expect(error.details?['domain'], 'files');
        expect(error.details?['recordIndex'], 0);
      }
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

