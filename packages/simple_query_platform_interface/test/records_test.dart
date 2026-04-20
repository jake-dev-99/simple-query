import 'package:flutter_test/flutter_test.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

void main() {
  group('ContactRecord.fromRecord', () {
    test('parses complete record', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '42',
        'displayName': 'Alice Example',
        'phones': ['+15551234567', '+15559876543'],
        'emails': ['alice@example.com'],
        'organization': 'Acme Corp',
        'updatedAt': '1700000000',
      });
      expect(record.id, '42');
      expect(record.displayName, 'Alice Example');
      expect(record.phones, ['+15551234567', '+15559876543']);
      expect(record.emails, ['alice@example.com']);
      expect(record.organization, 'Acme Corp');
      expect(record.updatedAt, '1700000000');
    });

    test('handles missing and null fields with defaults', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.displayName, '');
      expect(record.phones, isEmpty);
      expect(record.emails, isEmpty);
      expect(record.organization, isNull);
      expect(record.updatedAt, isNull);
    });

    test('coerces int id to string', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': 123,
        'displayName': 'Bob',
      });
      expect(record.id, '123');
    });

    test('throws on non-list phones', () {
      expect(
        () => ContactRecord.fromRecord(
          const <String, Object?>{'id': '1', 'phones': 'not-a-list'},
        ),
        throwsA(
          isA<SimpleQueryError>()
              .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery)
              .having((e) => e.message, 'message', contains('phones')),
        ),
      );
    });

    test('throws on scalar emails', () {
      expect(
        () => ContactRecord.fromRecord(
          const <String, Object?>{'id': '1', 'emails': 42},
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });

    test('throws on complex value for a string field', () {
      expect(
        () => ContactRecord.fromRecord(<String, Object?>{
          'id': <String, Object?>{'nested': 'map'},
        }),
        throwsA(
          isA<SimpleQueryError>()
              .having((e) => e.code, 'code', SimpleQueryErrorCode.invalidQuery),
        ),
      );
    });

    test('parses list of primitives to string list', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'phones': [123, 'text', true],
        'emails': ['a@b.com'],
      });
      expect(record.phones, ['123', 'text', 'true']);
      expect(record.emails, ['a@b.com']);
    });

    test('list with null entries drops them', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'phones': <Object?>['+1', null, '+2'],
      });
      expect(record.phones, ['+1', '+2']);
    });

    test('toString includes id and displayName', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'displayName': 'Test',
      });
      expect(record.toString(), contains('id: 1'));
      expect(record.toString(), contains('displayName: Test'));
    });
  });

  group('CalendarEventRecord.fromRecord', () {
    test('parses complete record', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '10',
        'title': 'Team Sync',
        'startAt': '1700000000',
        'endAt': '1700003600',
        'isAllDay': true,
        'calendarId': '3',
        'updatedAt': '1700003600',
      });
      expect(record.id, '10');
      expect(record.title, 'Team Sync');
      expect(record.startAt, '1700000000');
      expect(record.endAt, '1700003600');
      expect(record.isAllDay, true);
      expect(record.calendarId, '3');
      expect(record.updatedAt, '1700003600');
    });

    test('handles missing fields with defaults', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.title, '');
      expect(record.startAt, '');
      expect(record.endAt, '');
      expect(record.isAllDay, false);
      expect(record.calendarId, '');
      expect(record.updatedAt, isNull);
    });

    test('isAllDay coerces 1 to true (Android integer boolean)', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'isAllDay': 1,
      });
      expect(record.isAllDay, true);
    });

    test('isAllDay coerces 0 to false', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'isAllDay': 0,
      });
      expect(record.isAllDay, false);
    });

    test('isAllDay coerces "true"/"false" strings', () {
      expect(
        CalendarEventRecord.fromRecord(
          const <String, Object?>{'id': '1', 'isAllDay': 'true'},
        ).isAllDay,
        true,
      );
      expect(
        CalendarEventRecord.fromRecord(
          const <String, Object?>{'id': '1', 'isAllDay': 'FALSE'},
        ).isAllDay,
        false,
      );
    });

    test('isAllDay throws on uncoercible value', () {
      expect(
        () => CalendarEventRecord.fromRecord(
          const <String, Object?>{'id': '1', 'isAllDay': 'yes'},
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });
  });

  group('MediaRecord.fromRecord', () {
    test('parses complete record', () {
      final record = MediaRecord.fromRecord(const <String, Object?>{
        'id': '5',
        'uriOrPath': '/storage/photo.jpg',
        'mediaType': 'image',
        'mimeType': 'image/jpeg',
        'size': 4096,
        'createdAt': '2024-01-01T00:00:00Z',
        'modifiedAt': '2024-01-02T00:00:00Z',
      });
      expect(record.id, '5');
      expect(record.uriOrPath, '/storage/photo.jpg');
      expect(record.mediaType, 'image');
      expect(record.mimeType, 'image/jpeg');
      expect(record.size, 4096);
      expect(record.createdAt, '2024-01-01T00:00:00Z');
      expect(record.modifiedAt, '2024-01-02T00:00:00Z');
    });

    test('handles missing fields with defaults', () {
      final record = MediaRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.uriOrPath, '');
      expect(record.mediaType, 'other');
      expect(record.mimeType, isNull);
      expect(record.size, isNull);
    });

    test('int parsing for size from string', () {
      final record = MediaRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'size': '8192',
      });
      expect(record.size, 8192);
    });

    test('int parsing for size from double truncates', () {
      final record = MediaRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'size': 1024.7,
      });
      expect(record.size, 1024);
    });

    test('invalid size string returns null', () {
      final record = MediaRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'size': 'not-a-number',
      });
      expect(record.size, isNull);
    });

    test('size throws for complex value', () {
      expect(
        () => MediaRecord.fromRecord(
          <String, Object?>{'id': '1', 'size': <int>[1, 2]},
        ),
        throwsA(isA<SimpleQueryError>()),
      );
    });
  });

  group('FileRecord.fromRecord', () {
    test('parses complete record', () {
      final record = FileRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'path': '/tmp/test.txt',
        'name': 'test.txt',
        'isDirectory': false,
        'size': 512,
        'modifiedAt': '2024-01-01T00:00:00Z',
        'mimeType': 'text/plain',
        'type': 'file',
        'extension': 'txt',
        'modifiedEpochMs': 1704067200000,
      });
      expect(record.id, '1');
      expect(record.path, '/tmp/test.txt');
      expect(record.name, 'test.txt');
      expect(record.isDirectory, false);
      expect(record.size, 512);
      expect(record.modifiedAt, '2024-01-01T00:00:00Z');
      expect(record.mimeType, 'text/plain');
      expect(record.type, 'file');
      expect(record.extension, 'txt');
      expect(record.modifiedEpochMs, 1704067200000);
    });

    test('isDirectory accepts bool, int, and string', () {
      expect(
        FileRecord.fromRecord(const <String, Object?>{
          'id': '1',
          'path': '/d',
          'name': 'd',
          'isDirectory': true,
        }).isDirectory,
        true,
      );
      expect(
        FileRecord.fromRecord(const <String, Object?>{
          'id': '1',
          'path': '/d',
          'name': 'd',
          'isDirectory': 'true',
        }).isDirectory,
        true,
      );
      expect(
        FileRecord.fromRecord(const <String, Object?>{
          'id': '1',
          'path': '/d',
          'name': 'd',
          'isDirectory': 0,
        }).isDirectory,
        false,
      );
    });

    test('handles missing optional fields', () {
      final record = FileRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'path': '/tmp/a',
        'name': 'a',
        'isDirectory': false,
      });
      expect(record.size, isNull);
      expect(record.modifiedAt, isNull);
      expect(record.mimeType, isNull);
      expect(record.type, isNull);
      expect(record.extension, isNull);
      expect(record.modifiedEpochMs, isNull);
    });

    test('handles completely empty record', () {
      final record = FileRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.path, '');
      expect(record.name, '');
      expect(record.isDirectory, false);
    });
  });

  group('MessageRecord.fromRecord', () {
    test('parses complete record', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{
        'id': '100',
        'timestamp': '1700000000',
        'threadId': '5',
        'address': '+15551234567',
        'body': 'Hello world',
        'direction': 'inbox',
        'read': true,
      });
      expect(record.id, '100');
      expect(record.timestamp, '1700000000');
      expect(record.threadId, '5');
      expect(record.address, '+15551234567');
      expect(record.body, 'Hello world');
      expect(record.direction, 'inbox');
      expect(record.read, true);
    });

    test('read field coerces 1 to true', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'timestamp': '0',
        'read': 1,
      });
      expect(record.read, true);
    });

    test('read field is null when absent', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{});
      expect(record.read, isNull);
    });

    test('handles missing optional fields', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.timestamp, '');
      expect(record.threadId, isNull);
      expect(record.address, isNull);
      expect(record.body, isNull);
      expect(record.direction, isNull);
    });
  });

  group('CallRecord.fromRecord', () {
    test('parses complete record', () {
      final record = CallRecord.fromRecord(const <String, Object?>{
        'id': '7',
        'callType': 'incoming',
        'timestamp': '999999',
        'number': '+15551230000',
        'durationSec': 120,
        'name': 'Alice',
      });
      expect(record.id, '7');
      expect(record.callType, 'incoming');
      expect(record.timestamp, '999999');
      expect(record.number, '+15551230000');
      expect(record.durationSec, 120);
      expect(record.name, 'Alice');
    });

    test('default values for missing fields', () {
      final record = CallRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.callType, 'unknown');
      expect(record.timestamp, '');
      expect(record.number, isNull);
      expect(record.durationSec, isNull);
      expect(record.name, isNull);
    });

    test('durationSec parsed from string', () {
      final record = CallRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'durationSec': '60',
      });
      expect(record.durationSec, 60);
    });
  });
}
