import 'package:flutter_test/flutter_test.dart';
import 'package:simple_permissions_native/simple_permissions_native.dart';
import 'package:simple_query_android/src/generated/query.g.dart' as p;
import 'package:simple_query_android/src/simple_query_android_api.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart'
    as iface;

void main() {
  group('AndroidQueryPermissionResolver', () {
    test('maps media image uri to versioned images permission', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/images/media',
        write: false,
      );
      expect(permission, isA<VersionedPermission>());
      expect(permission!.identifier, 'versioned_images');
    });

    test('does not gate sms writes with SEND_SMS', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://sms',
        write: true,
      );
      expect(permission, isNull);
    });
  });

  test('query converts rows into v2 records', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {'id': 1, 'body': 'hi'},
        ],
        rowCount: 1,
        columnNames: ['id', 'body'],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final response = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.messages),
    );

    expect(response.records.length, 1);
    expect(response.records.first['id'], '1');
    expect(response.records.first['direction'], 'unknown');
    expect(
      iface.QueryDomainContracts.hasRequiredKeys(
        domain: iface.QueryDomain.messages,
        record: response.records.first,
      ),
      isTrue,
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.messages,
        record: response.records.first,
      ),
      isEmpty,
    );
  });

  test('query normalizes calls records to strict contract keys', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 7,
            'number': '+15551230000',
            'type': 1,
            'duration': 11,
            'date': 123456,
            'cached_name': 'Alice',
          },
        ],
        rowCount: 1,
        columnNames: ['_id', 'number', 'type', 'duration', 'date'],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final response = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.calls),
    );

    expect(response.records, hasLength(1));
    expect(
      iface.QueryDomainContracts.hasRequiredKeys(
        domain: iface.QueryDomain.calls,
        record: response.records.first,
      ),
      isTrue,
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.calls,
        record: response.records.first,
      ),
      isEmpty,
    );
  });

  test('query normalizes files and media records to strict contract keys',
      () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    host.queryResponse = p.QueryResponse(
      rows: [
        {
          '_id': 1,
          '_data': '/tmp/a.txt',
          '_display_name': 'a.txt',
          '_size': 12,
          'mime_type': 'text/plain',
        },
      ],
      rowCount: 1,
      columnNames: const [],
    );
    final files = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.files),
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.files,
        record: files.records.first,
      ),
      isEmpty,
    );

    host.queryResponse = p.QueryResponse(
      rows: [
        {
          '_id': 2,
          '_data': '/tmp/a.jpg',
          'media_type': 1,
          '_size': 22,
          'mime_type': 'image/jpeg',
          'date_added': 111,
        },
      ],
      rowCount: 1,
      columnNames: const [],
    );
    final media = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.media),
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.media,
        record: media.records.first,
      ),
      isEmpty,
    );
  });

  test('query normalizes contacts records to strict contract keys', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 11,
            'display_name': 'Alice Example',
            'phones': ['+15551234567'],
            'emails': ['alice@example.com'],
            'company': 'Example Co',
            'contact_last_updated_timestamp': 1111,
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final contacts = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.contacts),
    );

    expect(contacts.records, hasLength(1));
    expect(
      iface.QueryDomainContracts.hasRequiredKeys(
        domain: iface.QueryDomain.contacts,
        record: contacts.records.first,
      ),
      isTrue,
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.contacts,
        record: contacts.records.first,
      ),
      isEmpty,
    );
  });

  test('query normalizes calendar records to strict contract keys', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 21,
            'title': 'Team Sync',
            'dtstart': 1700000000,
            'dtend': 1700003600,
            'allDay': 0,
            'calendar_id': 3,
            'lastDate': 1700003600,
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final calendar = await api.query(
      const iface.QueryRequest(domain: iface.QueryDomain.calendar),
    );

    expect(calendar.records, hasLength(1));
    expect(
      iface.QueryDomainContracts.hasRequiredKeys(
        domain: iface.QueryDomain.calendar,
        record: calendar.records.first,
      ),
      isTrue,
    );
    expect(
      iface.QueryDomainContracts.unknownKeys(
        domain: iface.QueryDomain.calendar,
        record: calendar.records.first,
      ),
      isEmpty,
    );
  });

  test('capability matrix exposes full Android breadth', () async {
    final api = SimpleQueryAndroidApi(
      hostApi: _FakeHostApi(),
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final snapshot = await api.getCapabilities();
    final map = <iface.QueryDomain, iface.CapabilityDescriptor>{
      for (final capability in snapshot.capabilities)
        capability.domain: capability,
    };

    expect(map.length, iface.QueryDomain.values.length);
    for (final domain in iface.QueryDomain.values) {
      final capability = map[domain]!;
      expect(capability.canRead, isTrue);
      expect(capability.canWrite, isTrue);
      expect(capability.canObserve, isTrue);
    }
    expect(map[iface.QueryDomain.contacts]!.canStream, isFalse);
    expect(map[iface.QueryDomain.calendar]!.canStream, isFalse);
    expect(map[iface.QueryDomain.calls]!.canStream, isFalse);
    expect(map[iface.QueryDomain.media]!.canStream, isTrue);
    expect(map[iface.QueryDomain.files]!.canStream, isTrue);
    expect(map[iface.QueryDomain.messages]!.canStream, isTrue);
    expect(map[iface.QueryDomain.platformSpecific]!.canStream, isTrue);
    expect(snapshot.platformExtensions['android.provider'], isTrue);
  });

  test('batch maps mutation operations', () async {
    final host = _FakeHostApi();

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    await api.batch(
      const iface.BatchRequest(
        operations: <iface.MutationRequest>[
          iface.MutationRequest(
            domain: iface.QueryDomain.contacts,
            type: iface.MutationType.insert,
            values: <String, Object?>{'display_name': 'A'},
          ),
        ],
      ),
    );

    expect(host.applyBatchCalled, isTrue);
  });

  test('batch rejects mixed authorities before dispatch', () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    await expectLater(
      api.batch(
        const iface.BatchRequest(
          operations: <iface.MutationRequest>[
            iface.MutationRequest(
              domain: iface.QueryDomain.contacts,
              type: iface.MutationType.insert,
              values: <String, Object?>{'display_name': 'A'},
            ),
            iface.MutationRequest(
              domain: iface.QueryDomain.messages,
              type: iface.MutationType.delete,
            ),
          ],
        ),
      ),
      throwsA(
        isA<iface.SimpleQueryError>().having(
          (e) => e.code,
          'code',
          iface.SimpleQueryErrorCode.invalidQuery,
        ),
      ),
    );

    expect(host.applyBatchCalled, isFalse);
  });

  test('query omits nextOffset on terminal page', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {'_id': 1, 'body': 'hi'},
        ],
        rowCount: 1,
        columnNames: ['_id', 'body'],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final response = await api.query(
      const iface.QueryRequest(
        domain: iface.QueryDomain.messages,
        page: iface.QueryPage(limit: 2, offset: 0),
      ),
    );

    expect(response.nextOffset, isNull);
  });

  test('query returns nextOffset when page is full', () async {
    final host = _FakeHostApi()
      ..queryResponse = p.QueryResponse(
        rows: [
          {'_id': 1, 'body': 'hi'},
          {'_id': 2, 'body': 'there'},
        ],
        rowCount: 2,
        columnNames: ['_id', 'body'],
      );

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final response = await api.query(
      const iface.QueryRequest(
        domain: iface.QueryDomain.messages,
        page: iface.QueryPage(limit: 2, offset: 4),
      ),
    );

    expect(response.nextOffset, 6);
  });

  test('observe re-registers after cancel and relisten', () async {
    final host = _FakeHostApi();

    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final stream = api.observe(
      const iface.ObserveRequest(domain: iface.QueryDomain.messages),
    );

    final sub1 = stream.listen((_) {});
    await Future<void>.delayed(Duration.zero);
    await sub1.cancel();
    await Future<void>.delayed(Duration.zero);

    final sub2 = stream.listen((_) {});
    await Future<void>.delayed(Duration.zero);
    await sub2.cancel();

    expect(host.registerCount, 2);
    expect(host.unregisterCount, 2);
  });

  test('callExtension providerCall dispatches host call', () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final result = await api.callExtension(
      namespace: 'android.provider',
      method: 'providerCall',
      args: <String, Object?>{
        'authority': 'sms',
        'method': 'ping',
      },
    );

    expect(result, isNotNull);
    expect(result!['ok'], isTrue);
  });

  test('callExtension queryWithJoins returns row payload', () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final result = await api.callExtension(
      namespace: 'android.provider',
      method: 'queryWithJoins',
      args: <String, Object?>{
        'contentUri': 'content://sms',
        'joins': <Map<String, Object?>>[
          <String, Object?>{
            'name': 'parts',
            'contentUri': 'content://mms/part',
            'foreignKeyColumn': 'mid',
            'parentKeyColumn': '_id',
          },
        ],
      },
    );

    expect(result!['rowCount'], 1);
    expect(result['rows'], isA<List<Object?>>());
  });

  test('callExtension extractToFile returns path result', () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final result = await api.callExtension(
      namespace: 'android.provider',
      method: 'extractToFile',
      args: const <String, Object?>{
        'contentUri': 'content://mms/part/42',
      },
    );

    expect(result!['path'], '/tmp/extracted.bin');
  });

  test('callExtension rejects unsupported namespace', () async {
    final api = SimpleQueryAndroidApi(
      hostApi: _FakeHostApi(),
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    expect(
      () => api.callExtension(
        namespace: 'ios.photos',
        method: 'fetchAssetResources',
      ),
      throwsA(
        isA<iface.SimpleQueryError>()
            .having(
                (e) => e.code, 'code', iface.SimpleQueryErrorCode.notSupported)
            .having((e) => e.message, 'message',
                contains('simple_query: extension namespace')),
      ),
    );
  });

  test('callExtension rejects unknown method in android.provider namespace',
      () async {
    final api = SimpleQueryAndroidApi(
      hostApi: _FakeHostApi(),
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    expect(
      () => api.callExtension(
        namespace: 'android.provider',
        method: 'nope',
      ),
      throwsA(
        isA<iface.SimpleQueryError>()
            .having(
                (e) => e.code, 'code', iface.SimpleQueryErrorCode.notSupported)
            .having((e) => e.message, 'message',
                contains('simple_query: extension method nope')),
      ),
    );
  });

  test('extractToFile maps missing path to unavailable error', () async {
    final host = _FakeHostApi()..extractToFileResponse = null;
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    expect(
      () => api.callExtension(
        namespace: 'android.provider',
        method: 'extractToFile',
        args: const <String, Object?>{
          'contentUri': 'content://mms/part/404',
        },
      ),
      throwsA(
        isA<iface.SimpleQueryError>().having(
            (e) => e.code, 'code', iface.SimpleQueryErrorCode.unavailable),
      ),
    );
  });

  group('SQL filter generation via _selectionFromFilters', () {
    late _FakeHostApi host;
    late SimpleQueryAndroidApi api;

    setUp(() {
      host = _FakeHostApi()
        ..queryResponse = p.QueryResponse(
          rows: const [],
          rowCount: 0,
          columnNames: const [],
        );
      api = SimpleQueryAndroidApi(
        hostApi: host,
        registerFlutterApi: false,
        enforceAndroidPlatformCheck: false,
        checkPermission: (_) async => PermissionGrant.granted,
      );
    });

    // Tests below pass canonical field names (per Section 0 / P1) and
    // assert the *native* column names appear in the resulting SQL —
    // proving the canonical-to-native translation in
    // _AndroidFieldAliases.toNativeColumn fires symmetrically.

    test('equals operator generates = ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.messages,
          filters: [
            iface.QueryFilterCondition(
              field: 'address', // canonical == native
              operator: iface.QueryFilterOperator.equals,
              value: '+15551234567',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'address = ?');
      expect(host.lastQueryRequest!.selectionArgs, ['+15551234567']);
    });

    test('notEquals operator generates != ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'callType', // canonical → native `type`
              operator: iface.QueryFilterOperator.notEquals,
              value: '3',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'type != ?');
      expect(host.lastQueryRequest!.selectionArgs, ['3']);
    });

    test('greaterThan operator generates > ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'timestamp', // canonical → native `date`
              operator: iface.QueryFilterOperator.greaterThan,
              value: '1000',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'date > ?');
      expect(host.lastQueryRequest!.selectionArgs, ['1000']);
    });

    test('greaterThanOrEqual operator generates >= ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'timestamp',
              operator: iface.QueryFilterOperator.greaterThanOrEqual,
              value: '500',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'date >= ?');
      expect(host.lastQueryRequest!.selectionArgs, ['500']);
    });

    test('lessThan operator generates < ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'id', // canonical → native `_id`
              operator: iface.QueryFilterOperator.lessThan,
              value: '99',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, '_id < ?');
      expect(host.lastQueryRequest!.selectionArgs, ['99']);
    });

    test('lessThanOrEqual operator generates <= ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'id',
              operator: iface.QueryFilterOperator.lessThanOrEqual,
              value: '50',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, '_id <= ?');
      expect(host.lastQueryRequest!.selectionArgs, ['50']);
    });

    test('contains operator generates LIKE ? clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.messages,
          filters: [
            iface.QueryFilterCondition(
              field: 'body', // canonical == native
              operator: iface.QueryFilterOperator.contains,
              value: 'hello',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'body LIKE ?');
      expect(host.lastQueryRequest!.selectionArgs, ['%hello%']);
    });

    test('inList operator generates IN (...) clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'callType',
              operator: iface.QueryFilterOperator.inList,
              value: ['1', '2', '4'],
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'type IN (?, ?, ?)');
      expect(host.lastQueryRequest!.selectionArgs, ['1', '2', '4']);
    });

    test('multiple filters are joined with AND', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          filters: [
            iface.QueryFilterCondition(
              field: 'number',
              operator: iface.QueryFilterOperator.equals,
              value: '+1555',
            ),
            iface.QueryFilterCondition(
              field: 'callType',
              operator: iface.QueryFilterOperator.notEquals,
              value: '3',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'number = ? AND type != ?');
      expect(host.lastQueryRequest!.selectionArgs, ['+1555', '3']);
    });

    test('non-canonical field name throws invalidQuery', () async {
      // After Section 0, raw native column names are no longer accepted on
      // named domains — they must come in via QueryDomain.platformSpecific
      // or the typed canonical alias.
      await expectLater(
        api.query(
          const iface.QueryRequest(
            domain: iface.QueryDomain.calls,
            filters: [
              iface.QueryFilterCondition(
                field: 'type', // raw Android column, not canonical
                operator: iface.QueryFilterOperator.equals,
                value: '1',
              ),
            ],
          ),
        ),
        throwsA(
          isA<iface.SimpleQueryError>()
              .having((e) => e.code, 'code',
                  iface.SimpleQueryErrorCode.invalidQuery)
              .having((e) => e.details?['field'], 'details.field', 'type')
              .having((e) => e.details?['domain'], 'details.domain', 'calls'),
        ),
      );
    });

    test('SQL injection in platformSpecific is rejected by the regex',
        () async {
      // platformSpecific bypasses canonical validation, so the
      // _validFieldName regex remains the last line of defence.
      await expectLater(
        api.query(
          const iface.QueryRequest(
            domain: iface.QueryDomain.platformSpecific,
            platformData: <String, Object?>{
              'contentUri': 'content://com.example.test/data',
            },
            filters: [
              iface.QueryFilterCondition(
                field: "'; DROP TABLE",
                operator: iface.QueryFilterOperator.equals,
                value: 'bad',
              ),
            ],
          ),
        ),
        throwsA(
          isA<iface.SimpleQueryError>().having(
            (e) => e.code,
            'code',
            iface.SimpleQueryErrorCode.invalidQuery,
          ),
        ),
      );
    });

    test('field name with spaces in platformSpecific throws invalidQuery',
        () async {
      await expectLater(
        api.query(
          const iface.QueryRequest(
            domain: iface.QueryDomain.platformSpecific,
            platformData: <String, Object?>{
              'contentUri': 'content://com.example.test/data',
            },
            filters: [
              iface.QueryFilterCondition(
                field: 'bad field',
                operator: iface.QueryFilterOperator.equals,
                value: 'x',
              ),
            ],
          ),
        ),
        throwsA(
          isA<iface.SimpleQueryError>().having(
            (e) => e.code,
            'code',
            iface.SimpleQueryErrorCode.invalidQuery,
          ),
        ),
      );
    });

    test('valid field names with dots and underscores pass platformSpecific',
        () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.platformSpecific,
          platformData: <String, Object?>{
            'contentUri': 'content://com.example.test/data',
          },
          filters: [
            iface.QueryFilterCondition(
              field: 'contact_info.phone_number',
              operator: iface.QueryFilterOperator.equals,
              value: '555',
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.selection, 'contact_info.phone_number = ?');
    });
  });

  group('sort order generation via _sortOrderFrom', () {
    late _FakeHostApi host;
    late SimpleQueryAndroidApi api;

    setUp(() {
      host = _FakeHostApi()
        ..queryResponse = p.QueryResponse(
          rows: const [],
          rowCount: 0,
          columnNames: const [],
        );
      api = SimpleQueryAndroidApi(
        hostApi: host,
        registerFlutterApi: false,
        enforceAndroidPlatformCheck: false,
        checkPermission: (_) async => PermissionGrant.granted,
      );
    });

    test('ascending sort generates ASC clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          sort: [
            iface.QuerySort(
              field: 'timestamp', // canonical → native `date`
              direction: iface.QuerySortDirection.ascending,
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.sortOrder, 'date ASC');
    });

    test('descending sort generates DESC clause', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          sort: [
            iface.QuerySort(
              field: 'timestamp',
              direction: iface.QuerySortDirection.descending,
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.sortOrder, 'date DESC');
    });

    test('multiple sort fields are comma-separated and translated', () async {
      await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.calls,
          sort: [
            iface.QuerySort(
              field: 'timestamp',
              direction: iface.QuerySortDirection.descending,
            ),
            iface.QuerySort(
              field: 'id', // canonical → native `_id`
              direction: iface.QuerySortDirection.ascending,
            ),
          ],
        ),
      );
      expect(host.lastQueryRequest!.sortOrder, 'date DESC, _id ASC');
    });

    test('empty sort list defaults to _id ASC', () async {
      await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.messages),
      );
      expect(host.lastQueryRequest!.sortOrder, '_id ASC');
    });

    test('non-canonical sort field throws invalidQuery on a named domain',
        () async {
      await expectLater(
        api.query(
          const iface.QueryRequest(
            domain: iface.QueryDomain.calls,
            sort: [
              iface.QuerySort(field: 'date'), // raw, not canonical
            ],
          ),
        ),
        throwsA(
          isA<iface.SimpleQueryError>()
              .having((e) => e.code, 'code',
                  iface.SimpleQueryErrorCode.invalidQuery)
              .having((e) => e.details?['field'], 'details.field', 'date'),
        ),
      );
    });

    test('invalid sort field characters in platformSpecific throws invalidQuery',
        () async {
      await expectLater(
        api.query(
          const iface.QueryRequest(
            domain: iface.QueryDomain.platformSpecific,
            platformData: <String, Object?>{
              'contentUri': 'content://com.example.test/data',
            },
            sort: [
              iface.QuerySort(field: '1; DROP TABLE'),
            ],
          ),
        ),
        throwsA(
          isA<iface.SimpleQueryError>().having(
            (e) => e.code,
            'code',
            iface.SimpleQueryErrorCode.invalidQuery,
          ),
        ),
      );
    });
  });

  group('record normalization', () {
    late _FakeHostApi host;
    late SimpleQueryAndroidApi api;

    setUp(() {
      host = _FakeHostApi();
      api = SimpleQueryAndroidApi(
        hostApi: host,
        registerFlutterApi: false,
        enforceAndroidPlatformCheck: false,
        checkPermission: (_) async => PermissionGrant.granted,
      );
    });

    test('files domain maps _data to path and _display_name to name', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 1,
            '_data': '/storage/emulated/0/DCIM/photo.jpg',
            '_display_name': 'photo.jpg',
            '_size': 4096,
            'mime_type': 'image/jpeg',
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.files),
      );
      final record = result.records.first;
      expect(record['path'], '/storage/emulated/0/DCIM/photo.jpg');
      expect(record['name'], 'photo.jpg');
      expect(record['id'], '1');
      expect(record['size'], 4096);
      expect(record['mimeType'], 'image/jpeg');
      expect(record['isDirectory'], false);
    });

    test('contacts domain maps display_name to displayName', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 5,
            'display_name': 'Bob Smith',
            'phones': ['+15559876543'],
            'emails': ['bob@example.com'],
            'company': 'Acme',
            'contact_last_updated_timestamp': 9999,
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.contacts),
      );
      final record = result.records.first;
      expect(record['displayName'], 'Bob Smith');
      expect(record['phones'], ['+15559876543']);
      expect(record['emails'], ['bob@example.com']);
      expect(record['organization'], 'Acme');
    });

    test('calendar domain maps dtstart to startAt', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 10,
            'title': 'Meeting',
            'dtstart': 1700000000,
            'dtend': 1700003600,
            'allDay': 1,
            'calendar_id': 2,
            'lastDate': 1700003600,
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.calendar),
      );
      final record = result.records.first;
      expect(record['startAt'], '1700000000');
      expect(record['endAt'], '1700003600');
      expect(record['isAllDay'], true);
      expect(record['calendarId'], '2');
    });

    test('messages domain maps date to timestamp and type to direction',
        () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {'_id': 1, 'date': 123456, 'type': 1, 'body': 'hi', 'read': 1},
          {'_id': 2, 'date': 123457, 'type': 2, 'body': 'bye', 'read': 0},
          {'_id': 3, 'date': 123458, 'type': 3, 'body': 'draft'},
          {'_id': 4, 'date': 123459, 'type': 4, 'body': 'outbox'},
          {'_id': 5, 'date': 123460, 'type': 99, 'body': 'other'},
        ],
        rowCount: 5,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.messages),
      );
      expect(result.records[0]['timestamp'], '123456');
      expect(result.records[0]['direction'], 'inbox');
      expect(result.records[0]['read'], true);
      expect(result.records[1]['direction'], 'sent');
      expect(result.records[1]['read'], false);
      expect(result.records[2]['direction'], 'draft');
      expect(result.records[3]['direction'], 'outbox');
      expect(result.records[4]['direction'], 'unknown');
    });

    test('calls domain preserves call type string', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 7,
            'number': '+15551230000',
            'type': 1,
            'duration': 30,
            'date': 999999,
            'cached_name': 'Alice',
          },
        ],
        rowCount: 1,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.calls),
      );
      final record = result.records.first;
      expect(record['callType'], '1');
      expect(record['number'], '+15551230000');
      expect(record['durationSec'], 30);
      expect(record['timestamp'], '999999');
      expect(record['name'], 'Alice');
    });

    test('media domain maps media_type int codes to string types', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {
            '_id': 1,
            '_data': '/a.jpg',
            'media_type': 1,
            '_size': 100,
            'mime_type': 'image/jpeg',
            'date_added': 111
          },
          {
            '_id': 2,
            '_data': '/a.mp3',
            'media_type': 2,
            '_size': 200,
            'mime_type': 'audio/mpeg',
            'date_added': 222
          },
          {
            '_id': 3,
            '_data': '/a.mp4',
            'media_type': 3,
            '_size': 300,
            'mime_type': 'video/mp4',
            'date_added': 333
          },
          {
            '_id': 4,
            '_data': '/a.bin',
            'media_type': 99,
            '_size': 400,
            'mime_type': 'application/octet-stream',
            'date_added': 444
          },
        ],
        rowCount: 4,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(domain: iface.QueryDomain.media),
      );
      expect(result.records[0]['mediaType'], 'image');
      expect(result.records[1]['mediaType'], 'audio');
      expect(result.records[2]['mediaType'], 'video');
      expect(result.records[3]['mediaType'], 'other');
    });

    test('platformSpecific domain passes through unchanged', () async {
      host.queryResponse = p.QueryResponse(
        rows: [
          {'custom_key': 'custom_value', 'number': 42},
        ],
        rowCount: 1,
        columnNames: const [],
      );
      final result = await api.query(
        const iface.QueryRequest(
          domain: iface.QueryDomain.platformSpecific,
          platformData: {'contentUri': 'content://custom/table'},
        ),
      );
      final record = result.records.first;
      expect(record['custom_key'], 'custom_value');
      expect(record['number'], 42);
    });
  });

  group('AndroidQueryPermissionResolver', () {
    test('sms read uri maps to ReadSms', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://sms',
        write: false,
      );
      expect(permission, isA<ReadSms>());
    });

    test('contacts read uri maps to ReadContacts', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://com.android.contacts/contacts',
        write: false,
      );
      expect(permission, isA<ReadContacts>());
    });

    test('contacts write uri maps to WriteContacts', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://com.android.contacts/contacts',
        write: true,
      );
      expect(permission, isA<WriteContacts>());
    });

    test('call_log read uri maps to ReadCallLog', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://call_log/calls',
        write: false,
      );
      expect(permission, isA<ReadCallLog>());
    });

    test('call_log write uri maps to WriteCallLog', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://call_log/calls',
        write: true,
      );
      expect(permission, isA<WriteCallLog>());
    });

    test('calendar read uri maps to ReadCalendar', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://com.android.calendar/events',
        write: false,
      );
      expect(permission, isA<ReadCalendar>());
    });

    test('calendar write uri maps to WriteCalendar', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://com.android.calendar/events',
        write: true,
      );
      expect(permission, isA<WriteCalendar>());
    });

    test('media images uri maps to VersionedPermission.images', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/images/media',
        write: false,
      );
      expect(permission, isA<VersionedPermission>());
      expect(permission!.identifier, 'versioned_images');
    });

    test('media video uri maps to VersionedPermission.video', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/video/media',
        write: false,
      );
      expect(permission, isA<VersionedPermission>());
      expect(permission!.identifier, 'versioned_video');
    });

    test('media audio uri maps to VersionedPermission.audio', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/audio/media',
        write: false,
      );
      expect(permission, isA<VersionedPermission>());
      expect(permission!.identifier, 'versioned_audio');
    });

    test('media generic file uri maps to ReadExternalStorage', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/file',
        write: false,
      );
      expect(permission, isA<ReadExternalStorage>());
    });

    test('media write uri returns null', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://media/external/images/media',
        write: true,
      );
      expect(permission, isNull);
    });

    test('unknown uri returns null', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://com.example.custom/data',
        write: false,
      );
      expect(permission, isNull);
    });

    test('mms read uri maps to ReadSms', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://mms',
        write: false,
      );
      expect(permission, isA<ReadSms>());
    });

    test('mms write uri returns null', () {
      final permission = AndroidQueryPermissionResolver.permissionForUri(
        'content://mms',
        write: true,
      );
      expect(permission, isNull);
    });
  });

  test('openBinary and closeBinary dispatches host stream calls', () async {
    final host = _FakeHostApi();
    final api = SimpleQueryAndroidApi(
      hostApi: host,
      registerFlutterApi: false,
      enforceAndroidPlatformCheck: false,
      checkPermission: (_) async => PermissionGrant.granted,
    );

    final handle = await api.openBinary(
      const iface.BinaryRequest(
        domain: iface.QueryDomain.messages,
        entityType: 'mmsPart',
        recordId: '42',
      ),
    );

    expect(handle.handleId, 's');
    await api.closeBinary(handle.handleId);
    expect(host.closedStreams, contains('s'));
  });
}

class _FakeHostApi extends p.QueryHostApi {
  p.QueryResponse queryResponse =
      p.QueryResponse(rows: const [], rowCount: 0, columnNames: const []);
  int registerCount = 0;
  int unregisterCount = 0;
  bool applyBatchCalled = false;
  String? extractToFileResponse = '/tmp/extracted.bin';
  final List<String> closedStreams = <String>[];

  @override
  Future<List<p.BatchOperationResult?>> applyBatch(
      p.BatchRequest request) async {
    applyBatchCalled = true;
    return const <p.BatchOperationResult?>[];
  }

  p.QueryRequest? lastQueryRequest;

  @override
  Future<p.QueryResponse> query(p.QueryRequest request) async {
    lastQueryRequest = request;
    return queryResponse;
  }

  @override
  Future<String> registerObserver(p.ObserverRequest request) async {
    registerCount += 1;
    return 'obs_$registerCount';
  }

  @override
  Future<void> unregisterObserver(String observerId) async {
    unregisterCount += 1;
  }

  @override
  Future<void> unregisterAllObservers() async {}

  @override
  Future<p.JoinQueryResponse> queryWithJoins(
          p.JoinQueryRequest request) async =>
      p.JoinQueryResponse(
        rows: <p.JoinedRow?>[
          p.JoinedRow(
            data: <Object?, Object?>{'_id': 1, 'body': 'parent'},
            related: <Object?, Object?>{
              'parts': <Object?>[
                <Object?, Object?>{'_id': 10, 'mid': 1}
              ],
            },
          ),
        ],
        rowCount: 1,
      );

  @override
  Future<String?> insert(p.InsertRequest request) async => null;

  @override
  Future<int> bulkInsert(p.BulkInsertRequest request) async => 0;

  @override
  Future<int> update(p.UpdateRequest request) async => 0;

  @override
  Future<int> delete(p.DeleteRequest request) async => 0;

  @override
  Future<p.StreamDescriptor> openStream(String contentUri) async =>
      p.StreamDescriptor(streamId: 's', pipePath: '/tmp/p');

  @override
  Future<String?> extractToFile(String contentUri) async =>
      extractToFileResponse;

  @override
  Future<void> closeStream(String streamId) async {
    closedStreams.add(streamId);
  }

  @override
  Future<p.TypeResponse> getType(String contentUri) async => p.TypeResponse();

  @override
  Future<String?> canonicalize(String contentUri) async => null;

  @override
  Future<String?> uncanonicalize(String contentUri) async => null;

  @override
  Future<p.ProviderCallResponse> call(p.ProviderCallRequest request) async =>
      p.ProviderCallResponse(
        result: <String?, Object?>{
          'ok': true,
          'method': request.method,
        },
      );
}
