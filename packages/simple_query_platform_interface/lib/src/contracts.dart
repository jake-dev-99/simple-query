import 'models.dart';

abstract final class QueryDomainContracts {
  static const Map<QueryDomain, Set<String>> requiredKeys =
      <QueryDomain, Set<String>>{
    QueryDomain.contacts: <String>{
      'id',
      'displayName',
      'phones',
      'emails',
    },
    QueryDomain.calendar: <String>{
      'id',
      'title',
      'startAt',
      'endAt',
      'isAllDay',
      'calendarId',
    },
    QueryDomain.media: <String>{
      'id',
      'uriOrPath',
      'mediaType',
    },
    QueryDomain.files: <String>{
      'id',
      'path',
      'name',
      'isDirectory',
    },
    QueryDomain.messages: <String>{
      'id',
      'timestamp',
    },
    QueryDomain.calls: <String>{
      'id',
      'callType',
      'timestamp',
    },
    QueryDomain.platformSpecific: <String>{},
  };

  static const Map<QueryDomain, Set<String>> optionalKeys =
      <QueryDomain, Set<String>>{
    QueryDomain.contacts: <String>{
      'organization',
      'updatedAt',
    },
    QueryDomain.calendar: <String>{
      'updatedAt',
    },
    QueryDomain.media: <String>{
      'mimeType',
      'size',
      'createdAt',
      'modifiedAt',
    },
    QueryDomain.files: <String>{
      'size',
      'modifiedAt',
      'mimeType',
      'type',
      'extension',
      'modifiedEpochMs',
    },
    QueryDomain.messages: <String>{
      'threadId',
      'address',
      'body',
      'direction',
      'read',
    },
    QueryDomain.calls: <String>{
      'number',
      'durationSec',
      'name',
      // Added in 0.4.0 — see plan Section 0. Populated by Android from
      // CallLog columns `new`, `is_read`, `geocoded_location`,
      // `subscription_id`. iOS/macOS/desktop leave these absent.
      'isNew',
      'isRead',
      'geocodedLocation',
      'subscriptionId',
    },
    QueryDomain.platformSpecific: <String>{},
  };

  static List<String> missingKeys({
    required QueryDomain domain,
    required QueryRecord record,
  }) {
    final expected = requiredKeys[domain] ?? const <String>{};
    final missing = <String>[];
    for (final key in expected) {
      if (!record.containsKey(key)) {
        missing.add(key);
      }
    }
    return missing;
  }

  static bool hasRequiredKeys({
    required QueryDomain domain,
    required QueryRecord record,
  }) {
    return missingKeys(domain: domain, record: record).isEmpty;
  }

  static Set<String> allowedKeysFor(QueryDomain domain) {
    return <String>{
      ...(requiredKeys[domain] ?? const <String>{}),
      ...(optionalKeys[domain] ?? const <String>{}),
    };
  }

  static List<String> unknownKeys({
    required QueryDomain domain,
    required QueryRecord record,
  }) {
    if (domain == QueryDomain.platformSpecific) {
      return const <String>[];
    }
    final allowed = allowedKeysFor(domain);
    return record.keys
        .where((key) => !allowed.contains(key))
        .toList(growable: false);
  }
}

abstract final class CapabilityContracts {
  static bool isComplete(Iterable<CapabilityDescriptor> capabilities) {
    final domains = capabilities.map((item) => item.domain).toSet();
    return QueryDomain.values.every(domains.contains);
  }

  static List<QueryDomain> missingDomains(
      Iterable<CapabilityDescriptor> capabilities) {
    final domains = capabilities.map((item) => item.domain).toSet();
    return QueryDomain.values
        .where((domain) => !domains.contains(domain))
        .toList(growable: false);
  }
}
