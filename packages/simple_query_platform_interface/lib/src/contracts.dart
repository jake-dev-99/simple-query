import 'models.dart';

/// Declares the canonical required + optional record keys for each
/// [QueryDomain].
///
/// Platforms normalise their native row representation to this set of
/// keys before returning records — so a consumer can read e.g.
/// `record['callType']` on every platform, regardless of whether the
/// underlying column name was `type` (Android CallLog),
/// `CNCallKind` (iOS), or something else. OEM-specific extra columns
/// survive on the record as non-canonical keys and surface via
/// [ContactRecord.extras] / [CallRecord.extras] / etc.
///
/// This class is the **canonical contract**. It's also the input to
/// [QueryFieldCatalog] — consumers pass canonical names in filters /
/// sort / projection, which simple_query translates to each platform's
/// native column at query time.
///
/// See `docs/API_SEMANTICS.md` and `docs/DESIGN.md` (P2, P3) for why
/// the contract is AOSP-shaped and never per-OS-version.
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

/// Enforces that every [QueryDomain] is represented exactly once in a
/// [CapabilitySnapshot]. A platform that omits a domain — or reports it
/// twice — is a contract bug, and [RuntimeContractValidation] raises
/// `SimpleQueryError(unavailable, details: {missingDomains: [...]})`.
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
