// ignore_for_file: avoid_positional_boolean_parameters

import 'package:pigeon/pigeon.dart';

// ============================================================================
// Pigeon Configuration
// ============================================================================

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/src/generated/query.g.dart',
    kotlinOut: 'android/src/main/kotlin/io/simplezen/simple_query/Query.g.kt',
    kotlinOptions: KotlinOptions(
      package: 'io.simplezen.simple_query',
      errorClassName: 'QueryFlutterError',
    ),
    dartPackageName: 'simple_query',
  ),
)

// ============================================================================
// Enums
// ============================================================================

/// Type of content change observed by ContentObserver.
enum ContentChangeType {
  insert,
  update,
  delete,
  unknown,
}

/// Type of batch operation in applyBatch.
enum BatchOperationType {
  insert,
  update,
  delete,
  assertQuery,
}

// ============================================================================
// Data Classes - Query
// ============================================================================

/// Request for a query operation.
class QueryRequest {
  QueryRequest({
    required this.contentUri,
    this.projection,
    this.selection,
    this.selectionArgs,
    this.sortOrder,
    this.limit,
    this.offset,
  });

  /// Content URI to query (e.g., "content://sms", "content://contacts/data").
  final String contentUri;

  /// Columns to return. Null returns all columns.
  final List<String?>? projection;

  /// SQL WHERE clause (without "WHERE"). Use ? for parameters.
  final String? selection;

  /// Values to substitute for ? placeholders in selection.
  final List<String?>? selectionArgs;

  /// SQL ORDER BY clause (without "ORDER BY").
  final String? sortOrder;

  /// Maximum number of rows to return.
  final int? limit;

  /// Number of rows to skip (for pagination).
  final int? offset;
}

/// Response from a query operation.
class QueryResponse {
  QueryResponse({
    required this.rows,
    required this.rowCount,
    this.columnNames,
  });

  /// List of rows, each row is a map of column name to value.
  /// BLOB columns return null; use openStream() to retrieve binary data.
  /// Note: Uses Object? to avoid Pigeon CastList issues with nested generics.
  /// Each row is actually `Map<Object?, Object?>` - convert with `.cast<String?, Object?>()`.
  final List<Object?> rows;

  /// Total number of rows returned.
  final int rowCount;

  /// Column names in result set.
  final List<String?>? columnNames;
}

// ============================================================================
// Data Classes - Write Operations
// ============================================================================

/// Request to insert a row.
class InsertRequest {
  InsertRequest({
    required this.contentUri,
    required this.values,
  });

  final String contentUri;

  /// Column name to value map.
  final Map<String?, Object?> values;
}

/// Request to insert multiple rows.
class BulkInsertRequest {
  BulkInsertRequest({
    required this.contentUri,
    required this.valuesList,
  });

  final String contentUri;

  /// List of rows to insert.
  final List<Map<String?, Object?>?> valuesList;
}

/// Request to update rows.
class UpdateRequest {
  UpdateRequest({
    required this.contentUri,
    required this.values,
    this.selection,
    this.selectionArgs,
  });

  final String contentUri;

  /// Column name to new value map.
  final Map<String?, Object?> values;

  /// SQL WHERE clause.
  final String? selection;

  /// Values for ? placeholders.
  final List<String?>? selectionArgs;
}

/// Request to delete rows.
class DeleteRequest {
  DeleteRequest({
    required this.contentUri,
    this.selection,
    this.selectionArgs,
  });

  final String contentUri;

  /// SQL WHERE clause.
  final String? selection;

  /// Values for ? placeholders.
  final List<String?>? selectionArgs;
}

// ============================================================================
// Data Classes - Batch Operations
// ============================================================================

/// Single operation in a batch.
class BatchOperation {
  BatchOperation({
    required this.type,
    required this.contentUri,
    this.values,
    this.selection,
    this.selectionArgs,
    this.expectedCount,
    this.backReferenceIndex,
    this.backReferenceColumn,
  });

  final BatchOperationType type;
  final String contentUri;

  /// Values for insert/update.
  final Map<String?, Object?>? values;

  /// Selection for update/delete/assert.
  final String? selection;
  final List<String?>? selectionArgs;

  /// For assertQuery: expected row count.
  final int? expectedCount;

  /// Index of prior operation to reference for value.
  final int? backReferenceIndex;

  /// Column to use from back-referenced result.
  final String? backReferenceColumn;
}

/// Request for batch operations.
class BatchRequest {
  BatchRequest({
    required this.authority,
    required this.operations,
  });

  /// ContentProvider authority (e.g., "sms", "com.android.contacts").
  final String authority;

  /// List of operations to apply atomically.
  final List<BatchOperation?> operations;
}

/// Result of a single batch operation.
class BatchOperationResult {
  BatchOperationResult({
    this.uri,
    this.count,
  });

  /// URI of inserted row (for insert operations).
  final String? uri;

  /// Number of affected rows (for update/delete).
  final int? count;
}

// ============================================================================
// Data Classes - Join Queries
// ============================================================================

/// Specification for a joined/related query.
class JoinSpec {
  JoinSpec({
    required this.name,
    required this.contentUri,
    required this.foreignKeyColumn,
    required this.parentKeyColumn,
    this.projection,
    this.selection,
    this.selectionArgs,
    this.sortOrder,
  });

  /// Key name for this join in the result.
  final String name;

  /// Content URI for the related table.
  final String contentUri;

  /// Column in child table that references parent.
  final String foreignKeyColumn;

  /// Column in parent table (usually "_id").
  final String parentKeyColumn;

  /// Columns to return from child table.
  final List<String?>? projection;

  /// Additional filter for child rows.
  final String? selection;
  final List<String?>? selectionArgs;
  final String? sortOrder;
}

/// Request for a query with joins.
class JoinQueryRequest {
  JoinQueryRequest({
    required this.primary,
    required this.joins,
  });

  /// The primary/parent query.
  final QueryRequest primary;

  /// Related queries to join.
  final List<JoinSpec?> joins;
}

/// A row with nested related data.
class JoinedRow {
  JoinedRow({
    required this.data,
    required this.related,
  });

  /// Primary row data.
  /// Note: Actually `Map<Object?, Object?>` - convert keys with `.cast<String?, Object?>()`.
  final Object data;

  /// Related rows keyed by JoinSpec.name.
  /// Note: Uses Object to avoid Pigeon CastList issues with nested generics.
  final Object related;
}

/// Response for a join query.
class JoinQueryResponse {
  JoinQueryResponse({
    required this.rows,
    required this.rowCount,
  });

  final List<JoinedRow?> rows;
  final int rowCount;
}

// ============================================================================
// Data Classes - Streaming
// ============================================================================

/// Descriptor for an open stream.
class StreamDescriptor {
  StreamDescriptor({
    required this.streamId,
    required this.pipePath,
    this.mimeType,
    this.size,
  });

  /// Unique ID for this stream (for closing).
  final String streamId;

  /// Path to the read end of the named pipe.
  final String pipePath;

  /// MIME type of the content.
  final String? mimeType;

  /// Size in bytes if known.
  final int? size;
}

/// Error during streaming.
class StreamError {
  StreamError({
    required this.streamId,
    required this.contentUri,
    required this.message,
    this.bytesTransferred,
  });

  final String streamId;
  final String contentUri;
  final String message;
  final int? bytesTransferred;
}

// ============================================================================
// Data Classes - Observers
// ============================================================================

/// Event from ContentObserver.
class ContentChangeEvent {
  ContentChangeEvent({
    required this.observerId,
    required this.uri,
    required this.changeType,
    this.flags,
  });

  /// ID of the observer that fired.
  final String observerId;

  /// URI that changed.
  final String uri;

  /// Type of change.
  final ContentChangeType changeType;

  /// Additional flags from the observer.
  final int? flags;
}

/// Request to register an observer.
class ObserverRequest {
  ObserverRequest({
    required this.contentUri,
    this.notifyForDescendants = true,
  });

  final String contentUri;

  /// If true, also notify for changes to descendant URIs.
  final bool notifyForDescendants;
}

// ============================================================================
// Data Classes - Metadata
// ============================================================================

/// Response for getType().
class TypeResponse {
  TypeResponse({
    this.mimeType,
  });

  /// MIME type (e.g., "vnd.android.cursor.dir/sms").
  final String? mimeType;
}

/// Request for provider call().
class ProviderCallRequest {
  ProviderCallRequest({
    required this.authority,
    required this.method,
    this.arg,
    this.extras,
  });

  final String authority;
  final String method;
  final String? arg;
  final Map<String?, Object?>? extras;
}

/// Response from provider call().
class ProviderCallResponse {
  ProviderCallResponse({
    this.result,
  });

  final Map<String?, Object?>? result;
}

// ============================================================================
// Host API - Called from Dart
// ============================================================================

@HostApi()
abstract class QueryHostApi {
  // --- Query Operations ---

  @async
  QueryResponse query(QueryRequest request);

  @async
  JoinQueryResponse queryWithJoins(JoinQueryRequest request);

  // --- Write Operations ---

  /// Insert a row. Returns URI of inserted row.
  @async
  String? insert(InsertRequest request);

  /// Insert multiple rows. Returns count of inserted rows.
  @async
  int bulkInsert(BulkInsertRequest request);

  /// Update rows. Returns count of updated rows.
  @async
  int update(UpdateRequest request);

  /// Delete rows. Returns count of deleted rows.
  @async
  int delete(DeleteRequest request);

  // --- Batch Operations ---

  /// Apply batch operations atomically.
  @async
  List<BatchOperationResult?> applyBatch(BatchRequest request);

  // --- Streaming ---

  /// Open a stream for reading binary content.
  /// Returns StreamDescriptor with pipe path for zero-copy reading.
  @async
  StreamDescriptor openStream(String contentUri);

  /// Extract content to a temp file.
  /// Returns file:// URI to the temp file.
  @async
  String? extractToFile(String contentUri);

  /// Close an open stream and release resources.
  @async
  void closeStream(String streamId);

  // --- Observers ---

  /// Register a ContentObserver. Returns observer ID.
  @async
  String registerObserver(ObserverRequest request);

  /// Unregister a ContentObserver.
  @async
  void unregisterObserver(String observerId);

  /// Unregister all observers.
  @async
  void unregisterAllObservers();

  // --- Metadata ---

  /// Get MIME type for a content URI.
  @async
  TypeResponse getType(String contentUri);

  /// Canonicalize a URI for cross-device portability.
  @async
  String? canonicalize(String contentUri);

  /// Reverse canonicalization.
  @async
  String? uncanonicalize(String contentUri);

  /// Call a provider-defined method.
  @async
  ProviderCallResponse call(ProviderCallRequest request);

  // --- Permissions ---

  /// Returns true if the calling app holds [permissionName]
  /// (a fully-qualified Android permission identifier such as
  /// `android.permission.READ_CONTACTS`). Implemented via
  /// `Context.checkSelfPermission`. Synchronous on the native side; pigeon
  /// makes it async on the Dart side.
  @async
  bool hasPermission(String permissionName);

  /// Returns the device's `Build.VERSION.SDK_INT`. Used by the Dart-side
  /// permission catalog to pick between legacy and modern media permissions
  /// (`READ_EXTERNAL_STORAGE` vs `READ_MEDIA_*`).
  @async
  int getAndroidSdkInt();
}

// ============================================================================
// Flutter API - Called from Kotlin
// ============================================================================

@FlutterApi()
abstract class QueryFlutterApi {
  /// Called when observed content changes.
  void onContentChange(ContentChangeEvent event);

  /// Called when a stream encounters an error.
  void onStreamError(StreamError error);
}
