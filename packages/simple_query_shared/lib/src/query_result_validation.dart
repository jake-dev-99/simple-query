import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

/// Purpose: Validate projected records using the hosted core schema contract.
/// @param domain selects the required core fields.
/// @param result contains records to validate without changing their fields.
/// @param projection limits required-key checks to requested fields; null or
/// empty delegates complete-record validation to the platform interface.
/// @returns The unchanged result when its required fields in scope are present.
/// @throws SimpleQueryError when a required field in scope is missing.
QueryResult validateProjectedQueryResult({
  required QueryDomain domain,
  required QueryResult result,
  List<String>? projection,
}) {
  if (projection == null || projection.isEmpty) {
    return RuntimeContractValidation.validateQueryResult(
      domain: domain,
      result: result,
    );
  }
  if (domain == QueryDomain.platformSpecific) {
    return result;
  }

  final projectedFields = projection.toSet();
  for (var index = 0; index < result.records.length; index += 1) {
    final missingKeys = QueryDomainContracts.missingKeys(
      domain: domain,
      record: result.records[index],
    ).where(projectedFields.contains).toList();
    if (missingKeys.isEmpty) {
      continue;
    }
    throw SimpleQueryError(
      code: SimpleQueryErrorCode.unavailable,
      message: 'simple_query: backend returned records outside the '
          'documented ${domain.name} contract',
      domain: domain,
      operation: QueryOperation.read,
      details: <String, Object?>{
        'domain': domain.name,
        'recordIndex': index,
        'missingKeys': missingKeys,
      },
    );
  }
  return result;
}
