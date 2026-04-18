import 'package:collection/collection.dart';

/// A single row of data returned by a query, stored as field-name-to-value pairs.
typedef QueryRecord = Map<String, Object?>;

/// The category of device data you want to access.
enum QueryDomain {
  /// Address book contacts.
  contacts,

  /// Photos, videos, and audio files.
  media,

  /// Documents and other stored files.
  files,

  /// Calendar events and reminders.
  calendar,

  /// SMS, MMS, and chat messages.
  messages,

  /// Phone call history.
  calls,

  /// A domain not covered by the standard set; handled by a platform plugin.
  platformSpecific,
}

/// The kind of action being performed on device data.
enum QueryOperation {
  /// Fetch data without changing it.
  read,

  /// Create, update, or delete data.
  write,

  /// Watch for changes and receive notifications.
  observe,

  /// Open a continuous data stream (e.g. binary content).
  stream,
}

/// A comparison operator used to filter query results.
enum QueryFilterOperator {
  /// Value must exactly match.
  equals,

  /// Value must not match.
  notEquals,

  /// Value must be strictly greater.
  greaterThan,

  /// Value must be greater than or equal.
  greaterThanOrEqual,

  /// Value must be strictly less.
  lessThan,

  /// Value must be less than or equal.
  lessThanOrEqual,

  /// Value must contain the given substring or element.
  contains,

  /// Value must be one of the items in a provided list.
  inList,
}

/// The direction in which query results are sorted.
enum QuerySortDirection {
  /// Smallest to largest (A-Z, oldest first).
  ascending,

  /// Largest to smallest (Z-A, newest first).
  descending,
}

/// The kind of write operation to perform on device data.
enum MutationType {
  /// Add a new record.
  insert,

  /// Modify an existing record.
  update,

  /// Remove an existing record.
  delete,
}

/// The type of change detected when observing device data.
enum ObserveChangeType {
  /// A new record was added.
  insert,

  /// An existing record was modified.
  update,

  /// A record was removed.
  delete,

  /// The platform could not determine the change type.
  unknown,
}

const _deepEquals = DeepCollectionEquality();

/// A single filter rule that narrows which records a query returns.
///
/// Combine multiple conditions in a [QueryRequest] to build complex filters.
class QueryFilterCondition {
  const QueryFilterCondition({
    required this.field,
    required this.operator,
    this.value,
  });

  final String field;
  final QueryFilterOperator operator;
  final Object? value;

  QueryFilterCondition copyWith({
    String? field,
    QueryFilterOperator? operator,
    Object? value,
  }) {
    return QueryFilterCondition(
      field: field ?? this.field,
      operator: operator ?? this.operator,
      value: value ?? this.value,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is QueryFilterCondition &&
          field == other.field &&
          operator == other.operator &&
          _deepEquals.equals(value, other.value);

  @override
  int get hashCode => Object.hash(field, operator, _deepEquals.hash(value));

  @override
  String toString() =>
      'QueryFilterCondition(field: $field, operator: $operator, value: $value)';
}

/// A sorting rule that controls the order of query results.
class QuerySort {
  const QuerySort({
    required this.field,
    this.direction = QuerySortDirection.ascending,
  });

  final String field;
  final QuerySortDirection direction;

  QuerySort copyWith({
    String? field,
    QuerySortDirection? direction,
  }) {
    return QuerySort(
      field: field ?? this.field,
      direction: direction ?? this.direction,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is QuerySort &&
          field == other.field &&
          direction == other.direction;

  @override
  int get hashCode => Object.hash(field, direction);

  @override
  String toString() => 'QuerySort(field: $field, direction: $direction)';
}

/// Pagination settings that control which slice of results to return.
///
/// Supports two modes: offset-based (`limit` + `offset`) and cursor-based
/// (`limit` + `cursor`). When [cursor] is set, [offset] is ignored and the
/// query resumes from where the cursor left off. Get the next cursor from
/// [QueryResult.nextCursor].
class QueryPage {
  const QueryPage({
    this.limit,
    this.offset,
    this.cursor,
  });

  final int? limit;
  final int? offset;

  /// Opaque cursor for cursor-based pagination. When present, [offset] is
  /// ignored and the platform resumes from the position encoded in the cursor.
  /// Obtain this value from [QueryResult.nextCursor].
  final String? cursor;

  QueryPage copyWith({
    int? limit,
    int? offset,
    String? cursor,
  }) {
    return QueryPage(
      limit: limit ?? this.limit,
      offset: offset ?? this.offset,
      cursor: cursor ?? this.cursor,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is QueryPage &&
          limit == other.limit &&
          offset == other.offset &&
          cursor == other.cursor;

  @override
  int get hashCode => Object.hash(limit, offset, cursor);

  @override
  String toString() =>
      'QueryPage(limit: $limit, offset: $offset, cursor: $cursor)';
}

/// A request to read data from a specific [domain] with optional filters,
/// sorting, projection, and pagination.
class QueryRequest {
  const QueryRequest({
    required this.domain,
    this.entityType,
    this.filters = const <QueryFilterCondition>[],
    this.projection,
    this.sort = const <QuerySort>[],
    this.page,
    this.platformData,
  });

  final QueryDomain domain;
  final String? entityType;
  final List<QueryFilterCondition> filters;
  final List<String>? projection;
  final List<QuerySort> sort;
  final QueryPage? page;
  final Map<String, Object?>? platformData;

  QueryRequest copyWith({
    QueryDomain? domain,
    String? entityType,
    List<QueryFilterCondition>? filters,
    List<String>? projection,
    List<QuerySort>? sort,
    QueryPage? page,
    Map<String, Object?>? platformData,
  }) {
    return QueryRequest(
      domain: domain ?? this.domain,
      entityType: entityType ?? this.entityType,
      filters: filters ?? this.filters,
      projection: projection ?? this.projection,
      sort: sort ?? this.sort,
      page: page ?? this.page,
      platformData: platformData ?? this.platformData,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is QueryRequest &&
          domain == other.domain &&
          entityType == other.entityType &&
          _deepEquals.equals(filters, other.filters) &&
          _deepEquals.equals(projection, other.projection) &&
          _deepEquals.equals(sort, other.sort) &&
          page == other.page &&
          _deepEquals.equals(platformData, other.platformData);

  @override
  int get hashCode => Object.hash(
        domain,
        entityType,
        _deepEquals.hash(filters),
        _deepEquals.hash(projection),
        _deepEquals.hash(sort),
        page,
        _deepEquals.hash(platformData),
      );

  @override
  String toString() => 'QueryRequest(domain: $domain)';
}

/// The response from a query, containing matched records and pagination info.
class QueryResult {
  const QueryResult({
    required this.records,
    this.totalCount,
    this.nextOffset,
    this.nextCursor,
    this.metadata,
  });

  final List<QueryRecord> records;
  final int? totalCount;

  /// Next offset for offset-based pagination. Null when there are no more pages.
  final int? nextOffset;

  /// Opaque cursor for cursor-based pagination. Pass this to
  /// [QueryPage.cursor] to fetch the next page. Null when there are no more
  /// pages.
  final String? nextCursor;

  final Map<String, Object?>? metadata;

  QueryResult copyWith({
    List<QueryRecord>? records,
    int? totalCount,
    int? nextOffset,
    String? nextCursor,
    Map<String, Object?>? metadata,
  }) {
    return QueryResult(
      records: records ?? this.records,
      totalCount: totalCount ?? this.totalCount,
      nextOffset: nextOffset ?? this.nextOffset,
      nextCursor: nextCursor ?? this.nextCursor,
      metadata: metadata ?? this.metadata,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is QueryResult &&
          _deepEquals.equals(records, other.records) &&
          totalCount == other.totalCount &&
          nextOffset == other.nextOffset &&
          nextCursor == other.nextCursor &&
          _deepEquals.equals(metadata, other.metadata);

  @override
  int get hashCode => Object.hash(
        _deepEquals.hash(records),
        totalCount,
        nextOffset,
        nextCursor,
        _deepEquals.hash(metadata),
      );

  @override
  String toString() =>
      'QueryResult(records: ${records.length}, totalCount: $totalCount, nextOffset: $nextOffset, nextCursor: $nextCursor)';
}

/// A request to insert, update, or delete data in a specific [domain].
class MutationRequest {
  const MutationRequest({
    required this.domain,
    required this.type,
    this.entityType,
    this.values,
    this.filters = const <QueryFilterCondition>[],
    this.platformData,
  });

  final QueryDomain domain;
  final MutationType type;
  final String? entityType;
  final QueryRecord? values;
  final List<QueryFilterCondition> filters;
  final Map<String, Object?>? platformData;

  MutationRequest copyWith({
    QueryDomain? domain,
    MutationType? type,
    String? entityType,
    QueryRecord? values,
    List<QueryFilterCondition>? filters,
    Map<String, Object?>? platformData,
  }) {
    return MutationRequest(
      domain: domain ?? this.domain,
      type: type ?? this.type,
      entityType: entityType ?? this.entityType,
      values: values ?? this.values,
      filters: filters ?? this.filters,
      platformData: platformData ?? this.platformData,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is MutationRequest &&
          domain == other.domain &&
          type == other.type &&
          entityType == other.entityType &&
          _deepEquals.equals(values, other.values) &&
          _deepEquals.equals(filters, other.filters) &&
          _deepEquals.equals(platformData, other.platformData);

  @override
  int get hashCode => Object.hash(
        domain,
        type,
        entityType,
        _deepEquals.hash(values),
        _deepEquals.hash(filters),
        _deepEquals.hash(platformData),
      );

  @override
  String toString() => 'MutationRequest(domain: $domain, type: $type)';
}

/// The outcome of a mutation, including how many records were affected.
class MutationResult {
  const MutationResult({
    this.affectedCount,
    this.insertedId,
    this.metadata,
  });

  final int? affectedCount;
  final String? insertedId;
  final Map<String, Object?>? metadata;

  MutationResult copyWith({
    int? affectedCount,
    String? insertedId,
    Map<String, Object?>? metadata,
  }) {
    return MutationResult(
      affectedCount: affectedCount ?? this.affectedCount,
      insertedId: insertedId ?? this.insertedId,
      metadata: metadata ?? this.metadata,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is MutationResult &&
          affectedCount == other.affectedCount &&
          insertedId == other.insertedId &&
          _deepEquals.equals(metadata, other.metadata);

  @override
  int get hashCode =>
      Object.hash(affectedCount, insertedId, _deepEquals.hash(metadata));

  @override
  String toString() =>
      'MutationResult(affectedCount: $affectedCount, insertedId: $insertedId)';
}

/// A group of [MutationRequest]s to execute together as a single batch.
class BatchRequest {
  const BatchRequest({
    required this.operations,
    this.platformData,
  });

  final List<MutationRequest> operations;
  final Map<String, Object?>? platformData;

  BatchRequest copyWith({
    List<MutationRequest>? operations,
    Map<String, Object?>? platformData,
  }) {
    return BatchRequest(
      operations: operations ?? this.operations,
      platformData: platformData ?? this.platformData,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is BatchRequest &&
          _deepEquals.equals(operations, other.operations) &&
          _deepEquals.equals(platformData, other.platformData);

  @override
  int get hashCode => Object.hash(
        _deepEquals.hash(operations),
        _deepEquals.hash(platformData),
      );

  @override
  String toString() => 'BatchRequest(operations: ${operations.length})';
}

/// The combined outcome of a [BatchRequest], with one result per operation.
class BatchResult {
  const BatchResult({
    required this.results,
  });

  final List<MutationResult> results;

  BatchResult copyWith({
    List<MutationResult>? results,
  }) {
    return BatchResult(results: results ?? this.results);
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is BatchResult && _deepEquals.equals(results, other.results);

  @override
  int get hashCode => _deepEquals.hash(results);

  @override
  String toString() => 'BatchResult(results: ${results.length})';
}

/// Describes what operations (read, write, observe, stream) are supported
/// for a single [QueryDomain] on the current platform.
class CapabilityDescriptor {
  const CapabilityDescriptor({
    required this.domain,
    required this.canRead,
    required this.canWrite,
    required this.canObserve,
    required this.canStream,
    this.reason,
  });

  final QueryDomain domain;
  final bool canRead;
  final bool canWrite;
  final bool canObserve;
  final bool canStream;
  final String? reason;

  CapabilityDescriptor copyWith({
    QueryDomain? domain,
    bool? canRead,
    bool? canWrite,
    bool? canObserve,
    bool? canStream,
    String? reason,
  }) {
    return CapabilityDescriptor(
      domain: domain ?? this.domain,
      canRead: canRead ?? this.canRead,
      canWrite: canWrite ?? this.canWrite,
      canObserve: canObserve ?? this.canObserve,
      canStream: canStream ?? this.canStream,
      reason: reason ?? this.reason,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is CapabilityDescriptor &&
          domain == other.domain &&
          canRead == other.canRead &&
          canWrite == other.canWrite &&
          canObserve == other.canObserve &&
          canStream == other.canStream &&
          reason == other.reason;

  @override
  int get hashCode =>
      Object.hash(domain, canRead, canWrite, canObserve, canStream, reason);

  @override
  String toString() =>
      'CapabilityDescriptor(domain: $domain, read: $canRead, write: $canWrite, observe: $canObserve, stream: $canStream)';
}

/// A point-in-time summary of all capabilities available on this device.
class CapabilitySnapshot {
  const CapabilitySnapshot({
    required this.capabilities,
    this.platformExtensions = const <String, Object?>{},
  });

  final List<CapabilityDescriptor> capabilities;
  final Map<String, Object?> platformExtensions;

  CapabilitySnapshot copyWith({
    List<CapabilityDescriptor>? capabilities,
    Map<String, Object?>? platformExtensions,
  }) {
    return CapabilitySnapshot(
      capabilities: capabilities ?? this.capabilities,
      platformExtensions: platformExtensions ?? this.platformExtensions,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is CapabilitySnapshot &&
          _deepEquals.equals(capabilities, other.capabilities) &&
          _deepEquals.equals(platformExtensions, other.platformExtensions);

  @override
  int get hashCode => Object.hash(
        _deepEquals.hash(capabilities),
        _deepEquals.hash(platformExtensions),
      );

  @override
  String toString() =>
      'CapabilitySnapshot(capabilities: ${capabilities.length})';
}

/// A request to watch a [domain] for changes and receive [ObserveEvent]s.
class ObserveRequest {
  const ObserveRequest({
    required this.domain,
    this.entityType,
    this.filters = const <QueryFilterCondition>[],
    this.pollingInterval,
    this.platformData,
  });

  final QueryDomain domain;
  final String? entityType;
  final List<QueryFilterCondition> filters;
  final Duration? pollingInterval;
  final Map<String, Object?>? platformData;

  ObserveRequest copyWith({
    QueryDomain? domain,
    String? entityType,
    List<QueryFilterCondition>? filters,
    Duration? pollingInterval,
    Map<String, Object?>? platformData,
  }) {
    return ObserveRequest(
      domain: domain ?? this.domain,
      entityType: entityType ?? this.entityType,
      filters: filters ?? this.filters,
      pollingInterval: pollingInterval ?? this.pollingInterval,
      platformData: platformData ?? this.platformData,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ObserveRequest &&
          domain == other.domain &&
          entityType == other.entityType &&
          _deepEquals.equals(filters, other.filters) &&
          pollingInterval == other.pollingInterval &&
          _deepEquals.equals(platformData, other.platformData);

  @override
  int get hashCode => Object.hash(
        domain,
        entityType,
        _deepEquals.hash(filters),
        pollingInterval,
        _deepEquals.hash(platformData),
      );

  @override
  String toString() => 'ObserveRequest(domain: $domain)';
}

/// A notification that data in a watched [domain] has changed.
class ObserveEvent {
  const ObserveEvent({
    required this.domain,
    required this.changeType,
    required this.timestamp,
    this.entityType,
    this.ids = const <String>[],
    this.source,
    this.metadata,
  });

  final QueryDomain domain;
  final ObserveChangeType changeType;
  final DateTime timestamp;
  final String? entityType;
  final List<String> ids;
  final String? source;
  final Map<String, Object?>? metadata;

  ObserveEvent copyWith({
    QueryDomain? domain,
    ObserveChangeType? changeType,
    DateTime? timestamp,
    String? entityType,
    List<String>? ids,
    String? source,
    Map<String, Object?>? metadata,
  }) {
    return ObserveEvent(
      domain: domain ?? this.domain,
      changeType: changeType ?? this.changeType,
      timestamp: timestamp ?? this.timestamp,
      entityType: entityType ?? this.entityType,
      ids: ids ?? this.ids,
      source: source ?? this.source,
      metadata: metadata ?? this.metadata,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ObserveEvent &&
          domain == other.domain &&
          changeType == other.changeType &&
          timestamp == other.timestamp &&
          entityType == other.entityType &&
          _deepEquals.equals(ids, other.ids) &&
          source == other.source &&
          _deepEquals.equals(metadata, other.metadata);

  @override
  int get hashCode => Object.hash(
        domain,
        changeType,
        timestamp,
        entityType,
        _deepEquals.hash(ids),
        source,
        _deepEquals.hash(metadata),
      );

  @override
  String toString() =>
      'ObserveEvent(domain: $domain, changeType: $changeType, timestamp: $timestamp)';
}

/// A request to open binary content (e.g. a photo or file) for streaming.
class BinaryRequest {
  const BinaryRequest({
    required this.domain,
    this.entityType,
    this.recordId,
    this.platformData,
  });

  final QueryDomain domain;
  final String? entityType;
  final String? recordId;
  final Map<String, Object?>? platformData;

  BinaryRequest copyWith({
    QueryDomain? domain,
    String? entityType,
    String? recordId,
    Map<String, Object?>? platformData,
  }) {
    return BinaryRequest(
      domain: domain ?? this.domain,
      entityType: entityType ?? this.entityType,
      recordId: recordId ?? this.recordId,
      platformData: platformData ?? this.platformData,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is BinaryRequest &&
          domain == other.domain &&
          entityType == other.entityType &&
          recordId == other.recordId &&
          _deepEquals.equals(platformData, other.platformData);

  @override
  int get hashCode => Object.hash(
        domain,
        entityType,
        recordId,
        _deepEquals.hash(platformData),
      );

  @override
  String toString() => 'BinaryRequest(domain: $domain, recordId: $recordId)';
}

/// A handle to an open binary resource, providing its local path and metadata.
/// Close it with [SimpleQuery.closeBinary] when finished.
class BinaryContentHandle {
  const BinaryContentHandle({
    required this.handleId,
    required this.localPath,
    this.mimeType,
    this.size,
    this.metadata,
  });

  final String handleId;
  final String localPath;
  final String? mimeType;
  final int? size;
  final Map<String, Object?>? metadata;

  BinaryContentHandle copyWith({
    String? handleId,
    String? localPath,
    String? mimeType,
    int? size,
    Map<String, Object?>? metadata,
  }) {
    return BinaryContentHandle(
      handleId: handleId ?? this.handleId,
      localPath: localPath ?? this.localPath,
      mimeType: mimeType ?? this.mimeType,
      size: size ?? this.size,
      metadata: metadata ?? this.metadata,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is BinaryContentHandle &&
          handleId == other.handleId &&
          localPath == other.localPath &&
          mimeType == other.mimeType &&
          size == other.size &&
          _deepEquals.equals(metadata, other.metadata);

  @override
  int get hashCode => Object.hash(
        handleId,
        localPath,
        mimeType,
        size,
        _deepEquals.hash(metadata),
      );

  @override
  String toString() =>
      'BinaryContentHandle(handleId: $handleId, localPath: $localPath)';
}
