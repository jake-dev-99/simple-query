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

    test('parses non-list phones/emails as empty list', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'phones': 'not-a-list',
        'emails': 42,
      });
      // _toStringList returns empty for non-List values? Let's check.
      // Actually _toStringList: if value is List => map; else => <String>[]
      // 'not-a-list' is not a List, so should be empty.
      // But wait: the code says if value is List => map. 'not-a-list' is String,
      // not List. So => [].
      expect(record.phones, isEmpty);
      expect(record.emails, isEmpty);
    });

    test('parses list of mixed types to string list', () {
      final record = ContactRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'phones': [123, 'text', true],
        'emails': ['a@b.com'],
      });
      expect(record.phones, ['123', 'text', 'true']);
      expect(record.emails, ['a@b.com']);
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

    test('boolean coercion for isAllDay - false for non-true values', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'isAllDay': 'yes',
      });
      expect(record.isAllDay, false);
    });

    test('boolean coercion for isAllDay - true for literal true', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'isAllDay': true,
      });
      expect(record.isAllDay, true);
    });

    test('boolean coercion for isAllDay - false for 1', () {
      final record = CalendarEventRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'isAllDay': 1,
      });
      // record['isAllDay'] == true => 1 == true is false in Dart
      expect(record.isAllDay, false);
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

    test('int parsing for size from double', () {
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

    test('boolean for isDirectory - true only for literal true', () {
      final trueRecord = FileRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'path': '/tmp/dir',
        'name': 'dir',
        'isDirectory': true,
      });
      expect(trueRecord.isDirectory, true);

      final falseRecord = FileRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'path': '/tmp/dir',
        'name': 'dir',
        'isDirectory': 'true',
      });
      expect(falseRecord.isDirectory, false);
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

    test('boolean for read - false for non-true values', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'timestamp': '0',
        'read': 1,
      });
      expect(record.read, false);
    });

    test('boolean for read - true only for literal true', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{
        'id': '1',
        'timestamp': '0',
        'read': true,
      });
      expect(record.read, true);
    });

    test('handles missing optional fields', () {
      final record = MessageRecord.fromRecord(const <String, Object?>{});
      expect(record.id, '');
      expect(record.timestamp, '');
      expect(record.threadId, isNull);
      expect(record.address, isNull);
      expect(record.body, isNull);
      expect(record.direction, isNull);
      expect(record.read, false);
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
