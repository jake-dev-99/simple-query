import 'models.dart';

/// Typed view over a [QueryRecord] from the [QueryDomain.contacts] domain.
class ContactRecord {
  const ContactRecord._({
    required this.id,
    required this.displayName,
    required this.phones,
    required this.emails,
    this.organization,
    this.updatedAt,
  });

  factory ContactRecord.fromRecord(QueryRecord record) {
    return ContactRecord._(
      id: record['id']?.toString() ?? '',
      displayName: record['displayName']?.toString() ?? '',
      phones: _toStringList(record['phones']),
      emails: _toStringList(record['emails']),
      organization: record['organization']?.toString(),
      updatedAt: record['updatedAt']?.toString(),
    );
  }

  final String id;
  final String displayName;
  final List<String> phones;
  final List<String> emails;
  final String? organization;
  final String? updatedAt;

  @override
  String toString() => 'ContactRecord(id: $id, displayName: $displayName)';
}

/// Typed view over a [QueryRecord] from the [QueryDomain.calendar] domain.
class CalendarEventRecord {
  const CalendarEventRecord._({
    required this.id,
    required this.title,
    required this.startAt,
    required this.endAt,
    required this.isAllDay,
    required this.calendarId,
    this.updatedAt,
  });

  factory CalendarEventRecord.fromRecord(QueryRecord record) {
    return CalendarEventRecord._(
      id: record['id']?.toString() ?? '',
      title: record['title']?.toString() ?? '',
      startAt: record['startAt']?.toString() ?? '',
      endAt: record['endAt']?.toString() ?? '',
      isAllDay: record['isAllDay'] == true,
      calendarId: record['calendarId']?.toString() ?? '',
      updatedAt: record['updatedAt']?.toString(),
    );
  }

  final String id;
  final String title;
  final String startAt;
  final String endAt;
  final bool isAllDay;
  final String calendarId;
  final String? updatedAt;

  @override
  String toString() => 'CalendarEventRecord(id: $id, title: $title)';
}

/// Typed view over a [QueryRecord] from the [QueryDomain.media] domain.
class MediaRecord {
  const MediaRecord._({
    required this.id,
    required this.uriOrPath,
    required this.mediaType,
    this.mimeType,
    this.size,
    this.createdAt,
    this.modifiedAt,
  });

  factory MediaRecord.fromRecord(QueryRecord record) {
    return MediaRecord._(
      id: record['id']?.toString() ?? '',
      uriOrPath: record['uriOrPath']?.toString() ?? '',
      mediaType: record['mediaType']?.toString() ?? 'other',
      mimeType: record['mimeType']?.toString(),
      size: _asInt(record['size']),
      createdAt: record['createdAt']?.toString(),
      modifiedAt: record['modifiedAt']?.toString(),
    );
  }

  final String id;
  final String uriOrPath;
  final String mediaType;
  final String? mimeType;
  final int? size;
  final String? createdAt;
  final String? modifiedAt;

  @override
  String toString() => 'MediaRecord(id: $id, mediaType: $mediaType)';
}

/// Typed view over a [QueryRecord] from the [QueryDomain.files] domain.
class FileRecord {
  const FileRecord._({
    required this.id,
    required this.path,
    required this.name,
    required this.isDirectory,
    this.size,
    this.modifiedAt,
    this.mimeType,
    this.type,
    this.extension,
    this.modifiedEpochMs,
  });

  factory FileRecord.fromRecord(QueryRecord record) {
    return FileRecord._(
      id: record['id']?.toString() ?? '',
      path: record['path']?.toString() ?? '',
      name: record['name']?.toString() ?? '',
      isDirectory: record['isDirectory'] == true,
      size: _asInt(record['size']),
      modifiedAt: record['modifiedAt']?.toString(),
      mimeType: record['mimeType']?.toString(),
      type: record['type']?.toString(),
      extension: record['extension']?.toString(),
      modifiedEpochMs: _asInt(record['modifiedEpochMs']),
    );
  }

  final String id;
  final String path;
  final String name;
  final bool isDirectory;
  final int? size;
  final String? modifiedAt;
  final String? mimeType;
  final String? type;
  final String? extension;
  final int? modifiedEpochMs;

  @override
  String toString() => 'FileRecord(id: $id, path: $path)';
}

/// Typed view over a [QueryRecord] from the [QueryDomain.messages] domain.
class MessageRecord {
  const MessageRecord._({
    required this.id,
    required this.timestamp,
    this.threadId,
    this.address,
    this.body,
    this.direction,
    this.read,
  });

  factory MessageRecord.fromRecord(QueryRecord record) {
    return MessageRecord._(
      id: record['id']?.toString() ?? '',
      timestamp: record['timestamp']?.toString() ?? '',
      threadId: record['threadId']?.toString(),
      address: record['address']?.toString(),
      body: record['body']?.toString(),
      direction: record['direction']?.toString(),
      read: record['read'] == true,
    );
  }

  final String id;
  final String timestamp;
  final String? threadId;
  final String? address;
  final String? body;
  final String? direction;
  final bool? read;

  @override
  String toString() => 'MessageRecord(id: $id, timestamp: $timestamp)';
}

/// Typed view over a [QueryRecord] from the [QueryDomain.calls] domain.
class CallRecord {
  const CallRecord._({
    required this.id,
    required this.callType,
    required this.timestamp,
    this.number,
    this.durationSec,
    this.name,
  });

  factory CallRecord.fromRecord(QueryRecord record) {
    return CallRecord._(
      id: record['id']?.toString() ?? '',
      callType: record['callType']?.toString() ?? 'unknown',
      timestamp: record['timestamp']?.toString() ?? '',
      number: record['number']?.toString(),
      durationSec: _asInt(record['durationSec']),
      name: record['name']?.toString(),
    );
  }

  final String id;
  final String callType;
  final String timestamp;
  final String? number;
  final int? durationSec;
  final String? name;

  @override
  String toString() => 'CallRecord(id: $id, callType: $callType)';
}

List<String> _toStringList(Object? value) {
  if (value is List) {
    return value.map((item) => item.toString()).toList(growable: false);
  }
  return const <String>[];
}

int? _asInt(Object? value) {
  if (value is int) return value;
  if (value is num) return value.toInt();
  if (value is String) return int.tryParse(value);
  return null;
}
