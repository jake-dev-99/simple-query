import 'contracts.dart';
import 'exceptions.dart';
import 'field_catalog.dart';
import 'models.dart';

/// Enforces the documented contracts of request/response models at runtime.
///
/// Dart's type system covers the shape of each model; these checks cover
/// domain-level invariants that the type system can't express (e.g. an
/// `insert` mutation must carry non-empty values, a `QueryResult` must
/// include every required key for its domain).
///
/// Every violation throws [SimpleQueryError] with an actionable `details`
/// map so callers can log, retry, or surface a useful error.
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
            'documented ${domain.name} contract',
        domain: domain,
        operation: operation,
        details: <String, Object?>{
          'domain': domain.name,
          'recordIndex': index,
          'missingKeys': missingKeys,
        },
      );
    }

    return result;
  }

  /// Enforces that every filter, sort, and projection field on [request] is
  /// part of the canonical schema for its domain. Skips validation for
  /// [QueryDomain.platformSpecific] (the escape hatch).
  ///
  /// Surfaces typos and raw native column names with a clear
  /// [SimpleQueryError] instead of letting them silently produce empty
  /// result sets (in-memory filters) or cryptic SQL errors (Android).
  static QueryRequest validateQueryRequest(QueryRequest request) {
    QueryFieldCatalog.ensureAllKnown(
      domain: request.domain,
      fields: request.filters.map((filter) => filter.field),
    );
    QueryFieldCatalog.ensureAllKnown(
      domain: request.domain,
      fields: request.sort.map((sort) => sort.field),
    );
    final projection = request.projection;
    if (projection != null) {
      QueryFieldCatalog.ensureAllKnown(
        domain: request.domain,
        fields: projection,
      );
    }
    return request;
  }

  /// Enforces that insert/update mutations carry non-empty `values`, and
  /// that every filter field (used by update/delete) is a canonical field
  /// for the mutation's domain.
  static MutationRequest validateMutationRequest(MutationRequest request) {
    QueryFieldCatalog.ensureAllKnown(
      domain: request.domain,
      fields: request.filters.map((filter) => filter.field),
    );
    switch (request.type) {
      case MutationType.insert:
      case MutationType.update:
        final values = request.values;
        if (values == null || values.isEmpty) {
          throw SimpleQueryError(
            code: SimpleQueryErrorCode.invalidQuery,
            message:
                'simple_query: ${request.type.name} mutation requires non-empty values',
            domain: request.domain,
            operation: QueryOperation.write,
            details: <String, Object?>{
              'domain': request.domain.name,
              'type': request.type.name,
            },
          );
        }
      case MutationType.delete:
        // `values` may be null; filters drive the deletion scope.
        break;
    }
    return request;
  }

  /// Enforces that a batch carries at least one operation.
  ///
  /// Does **not** cascade to per-operation [validateMutationRequest]. Batch
  /// semantics are "sequentialBestEffort" — a malformed operation must not
  /// abort the whole batch; it is expected to fail at execution time and
  /// record its error in its own result's `metadata.error`. Platforms that
  /// want upfront per-op validation can call [validateMutationRequest]
  /// themselves.
  static BatchRequest validateBatchRequest(BatchRequest request) {
    if (request.operations.isEmpty) {
      throw const SimpleQueryError(
        code: SimpleQueryErrorCode.invalidQuery,
        message:
            'simple_query: batch request must contain at least one operation',
        operation: QueryOperation.write,
        details: <String, Object?>{'operationsCount': 0},
      );
    }
    return request;
  }

  /// Enforces that a polling interval, when present, is strictly positive,
  /// and that every filter field is canonical for the request's domain.
  static ObserveRequest validateObserveRequest(ObserveRequest request) {
    QueryFieldCatalog.ensureAllKnown(
      domain: request.domain,
      fields: request.filters.map((filter) => filter.field),
    );
    final interval = request.pollingInterval;
    if (interval != null && interval <= Duration.zero) {
      throw SimpleQueryError(
        code: SimpleQueryErrorCode.invalidQuery,
        message: 'simple_query: observe pollingInterval must be positive',
        domain: request.domain,
        operation: QueryOperation.observe,
        details: <String, Object?>{
          'domain': request.domain.name,
          'pollingIntervalMicros': interval.inMicroseconds,
        },
      );
    }
    return request;
  }

  /// Enforces that a binary request identifies the resource it wants to
  /// open (either via `recordId` or a platform-specific hook in
  /// `platformData`).
  static BinaryRequest validateBinaryRequest(BinaryRequest request) {
    final hasRecordId =
        request.recordId != null && request.recordId!.isNotEmpty;
    final hasPlatformData =
        request.platformData != null && request.platformData!.isNotEmpty;
    if (!hasRecordId && !hasPlatformData) {
      throw SimpleQueryError(
        code: SimpleQueryErrorCode.invalidQuery,
        message:
            'simple_query: binary request requires recordId or platformData',
        domain: request.domain,
        operation: QueryOperation.stream,
        details: <String, Object?>{'domain': request.domain.name},
      );
    }
    return request;
  }
}
