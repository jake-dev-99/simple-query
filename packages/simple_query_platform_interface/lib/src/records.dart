import 'exceptions.dart';
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
      id: _requiredString(record, 'id'),
      displayName: _requiredString(record, 'displayName'),
      phones: _stringList(record, 'phones'),
      emails: _stringList(record, 'emails'),
      organization: _optionalString(record, 'organization'),
      updatedAt: _optionalString(record, 'updatedAt'),
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
      id: _requiredString(record, 'id'),
      title: _requiredString(record, 'title'),
      startAt: _requiredString(record, 'startAt'),
      endAt: _requiredString(record, 'endAt'),
      isAllDay: _boolOrFalse(record, 'isAllDay'),
      calendarId: _requiredString(record, 'calendarId'),
      updatedAt: _optionalString(record, 'updatedAt'),
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
      id: _requiredString(record, 'id'),
      uriOrPath: _requiredString(record, 'uriOrPath'),
      mediaType: _requiredString(record, 'mediaType', fallback: 'other'),
      mimeType: _optionalString(record, 'mimeType'),
      size: _optionalInt(record, 'size'),
      createdAt: _optionalString(record, 'createdAt'),
      modifiedAt: _optionalString(record, 'modifiedAt'),
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
      id: _requiredString(record, 'id'),
      path: _requiredString(record, 'path'),
      name: _requiredString(record, 'name'),
      isDirectory: _boolOrFalse(record, 'isDirectory'),
      size: _optionalInt(record, 'size'),
      modifiedAt: _optionalString(record, 'modifiedAt'),
      mimeType: _optionalString(record, 'mimeType'),
      type: _optionalString(record, 'type'),
      extension: _optionalString(record, 'extension'),
      modifiedEpochMs: _optionalInt(record, 'modifiedEpochMs'),
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
      id: _requiredString(record, 'id'),
      timestamp: _requiredString(record, 'timestamp'),
      threadId: _optionalString(record, 'threadId'),
      address: _optionalString(record, 'address'),
      body: _optionalString(record, 'body'),
      direction: _optionalString(record, 'direction'),
      read: _optionalBool(record, 'read'),
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
      id: _requiredString(record, 'id'),
      callType: _requiredString(record, 'callType', fallback: 'unknown'),
      timestamp: _requiredString(record, 'timestamp'),
      number: _optionalString(record, 'number'),
      durationSec: _optionalInt(record, 'durationSec'),
      name: _optionalString(record, 'name'),
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

// ---------------------------------------------------------------------------
// Coercion helpers.
//
// The platform channel delivers records as `Map<String, Object?>` with values
// whose runtime types vary by platform (Kotlin, Swift, C++). These helpers
// normalise primitives consistently and throw [SimpleQueryError] with
// [SimpleQueryErrorCode.invalidQuery] when a field is present but has a shape
// the contract does not allow — surfacing backend contract violations loudly
// instead of silently coercing garbage.
// ---------------------------------------------------------------------------

String _requiredString(QueryRecord record, String field, {String fallback = ''}) {
  final value = record[field];
  if (value == null) return fallback;
  return _coerceString(value, field);
}

String? _optionalString(QueryRecord record, String field) {
  final value = record[field];
  if (value == null) return null;
  return _coerceString(value, field);
}

String _coerceString(Object value, String field) {
  if (value is String) return value;
  if (value is num || value is bool) return value.toString();
  throw SimpleQueryError(
    code: SimpleQueryErrorCode.invalidQuery,
    message:
        'Record field "$field" expected a String/num/bool, got ${value.runtimeType}',
    details: <String, Object?>{'field': field, 'runtimeType': '${value.runtimeType}'},
  );
}

bool _boolOrFalse(QueryRecord record, String field) {
  return _optionalBool(record, field) ?? false;
}

bool? _optionalBool(QueryRecord record, String field) {
  final value = record[field];
  if (value == null) return null;
  if (value is bool) return value;
  if (value is num) {
    if (value == 0) return false;
    if (value == 1) return true;
  }
  if (value is String) {
    switch (value.toLowerCase()) {
      case 'true':
      case '1':
        return true;
      case 'false':
      case '0':
        return false;
    }
  }
  throw SimpleQueryError(
    code: SimpleQueryErrorCode.invalidQuery,
    message:
        'Record field "$field" expected a bool-coercible value, got ${value.runtimeType}',
    details: <String, Object?>{'field': field, 'value': '$value'},
  );
}

int? _optionalInt(QueryRecord record, String field) {
  final value = record[field];
  if (value == null) return null;
  if (value is int) return value;
  if (value is num) return value.toInt();
  if (value is String) return int.tryParse(value);
  throw SimpleQueryError(
    code: SimpleQueryErrorCode.invalidQuery,
    message:
        'Record field "$field" expected an int-coercible value, got ${value.runtimeType}',
    details: <String, Object?>{'field': field, 'runtimeType': '${value.runtimeType}'},
  );
}

List<String> _stringList(QueryRecord record, String field) {
  final value = record[field];
  if (value == null) return const <String>[];
  if (value is! List) {
    throw SimpleQueryError(
      code: SimpleQueryErrorCode.invalidQuery,
      message:
          'Record field "$field" expected a List, got ${value.runtimeType}',
      details: <String, Object?>{'field': field, 'runtimeType': '${value.runtimeType}'},
    );
  }
  return <String>[
    for (final Object? item in value)
      if (item != null) _coerceString(item, '$field[]'),
  ];
}
