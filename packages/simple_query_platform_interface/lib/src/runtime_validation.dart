import 'contracts.dart';
import 'exceptions.dart';
import 'models.dart';

abstract final class RuntimeContractValidation {
  static CapabilitySnapshot validateCapabilitySnapshot(
    CapabilitySnapshot snapshot, {
    QueryOperation operation = QueryOperation.read,
  }) {
    final missingDomains =
        CapabilityContracts.missingDomains(snapshot.capabilities);
    if (missingDomains.isNotEmpty) {
      throw SimpleQueryError(
        code: SimpleQueryErrorCode.unavailable,
        message: 'simple_query: capability snapshot is incomplete',
        operation: operation,
        details: <String, Object?>{
          'missingDomains': missingDomains
              .map((domain) => domain.name)
              .toList(growable: false),
        },
      );
    }
    return snapshot;
  }

  static QueryResult validateQueryResult({
    required QueryDomain domain,
    required QueryResult result,
    QueryOperation operation = QueryOperation.read,
  }) {
    if (domain == QueryDomain.platformSpecific) {
      return result;
    }

    for (var index = 0; index < result.records.length; index += 1) {
      final record = result.records[index];
      final missingKeys =
          QueryDomainContracts.missingKeys(domain: domain, record: record);

      if (missingKeys.isEmpty) {
        continue;
      }

      throw SimpleQueryError(
        code: SimpleQueryErrorCode.unavailable,
        message: 'simple_query: backend returned records outside the '
            'documented $domain contract',
        domain: domain,
        operation: operation,
        details: <String, Object?>{
          'recordIndex': index,
          'missingKeys': missingKeys,
        },
      );
    }

    return result;
  }
}
