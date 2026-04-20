import 'contracts.dart';
import 'exceptions.dart';
import 'models.dart';

/// The canonical field vocabulary each [QueryDomain] accepts for filters,
/// sort, projection, and mutation values.
///
/// simple_query consumers speak canonical names (`callType`, `timestamp`,
/// `durationSec`, ...). Each platform package owns its own
/// canonical-to-native translation for the backends it talks to (Android
/// ContentResolver columns, Apple EventKit / Contacts keys, etc.); the
/// translation happens inside the platform package, not here.
///
/// This class is the single source of truth for *what* is a valid canonical
/// field. It intentionally does not know about native column names —
/// that stays in each platform package's own alias table so the layering
/// remains clean.
///
/// The `platformSpecific` domain is the escape hatch: fields pass through
/// without validation so callers can hit arbitrary content providers.
abstract final class QueryFieldCatalog {
  /// All canonical fields recognised for [domain]: the union of
  /// [QueryDomainContracts.requiredKeys] and
  /// [QueryDomainContracts.optionalKeys]. Empty for
  /// [QueryDomain.platformSpecific].
  static Set<String> canonicalFields(QueryDomain domain) {
    if (domain == QueryDomain.platformSpecific) return const <String>{};
    return QueryDomainContracts.allowedKeysFor(domain);
  }

  /// Throws [SimpleQueryError] with `code: invalidQuery` if [canonical] is
  /// not part of [domain]'s canonical schema. No-op for
  /// [QueryDomain.platformSpecific].
  ///
  /// Platform packages call this from their filter, sort, projection, and
  /// mutation-values translators to surface typos and schema drift with a
  /// clear error instead of forwarding the bad name to the native backend
  /// and getting a cryptic SQL error.
  static void ensureKnown({
    required QueryDomain domain,
    required String canonical,
  }) {
    if (domain == QueryDomain.platformSpecific) return;
    final allowed = canonicalFields(domain);
    if (allowed.contains(canonical)) return;
    throw SimpleQueryError(
      code: SimpleQueryErrorCode.invalidQuery,
      message: 'simple_query: "$canonical" is not a canonical field for '
          'domain ${domain.name}. '
          'Allowed: ${allowed.toList(growable: false)..sort()}',
      domain: domain,
      details: <String, Object?>{
        'domain': domain.name,
        'field': canonical,
        'allowed': allowed.toList(growable: false)..sort(),
      },
    );
  }

  /// Convenience: validate every field in [fields] against [domain]. Returns
  /// the list unchanged on success; throws on the first unknown field.
  static List<String> ensureAllKnown({
    required QueryDomain domain,
    required Iterable<String> fields,
  }) {
    for (final field in fields) {
      ensureKnown(domain: domain, canonical: field);
    }
    return fields.toList(growable: false);
  }
}
