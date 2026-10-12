package io.simplezen.simple_query

import android.content.ContentProviderClient
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch

/**
 * Implementation of the Pigeon-generated QueryHostApi.
 *
 * Uses callback-based async pattern as required by Pigeon v22+.
 * All operations run on [Dispatchers.IO] (injectable for tests) and return
 * results via callback. Every failure reaches Dart through [HostErrors], so
 * its Pigeon code is one of the `docs/ERROR_MAPPING.md` vocabulary codes.
 */
class QueryHostApiImpl(
    private val context: Context,
    private val flutterApi: QueryFlutterApi,
    ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) : QueryHostApi {

    private val scope = CoroutineScope(SupervisorJob() + ioDispatcher)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val streamManager = StreamManager(context, flutterApi, mainHandler)
    private val observerRegistry = ObserverRegistry(context, flutterApi, mainHandler)

    private val contentResolver get() = context.contentResolver

    // -------------------------------------------------------------------------
    // Query
    // -------------------------------------------------------------------------

    override fun query(request: QueryRequest, callback: (Result<QueryResponse>) -> Unit) =
        launchHostCall(callback) {
            val uri = Uri.parse(request.contentUri)

            val cursor = contentResolver.query(
                uri,
                request.projection?.filterNotNull()?.toTypedArray(),
                request.selection,
                request.selectionArgs?.filterNotNull()?.toTypedArray(),
                buildSortOrderWithLimit(request.sortOrder, request.limit, request.offset)
            ) ?: throw nullCursorError(request.contentUri)

            cursor.use { c ->
                val columns = c.columnNames.toList()
                val rows = mutableListOf<Map<String?, Any?>?>()

                while (c.moveToNext()) {
                    rows.add(cursorRowToMap(c))
                }

                QueryResponse(
                    columnNames = columns,
                    rows = rows,
                    rowCount = rows.size.toLong()
                )
            }
        }

    override fun queryWithJoins(request: JoinQueryRequest, callback: (Result<JoinQueryResponse>) -> Unit) =
        launchHostCall(callback) {
            // Execute primary query synchronously (we're already on IO)
            val primaryUri = Uri.parse(request.primary.contentUri)
            val primaryCursor = contentResolver.query(
                primaryUri,
                request.primary.projection?.filterNotNull()?.toTypedArray(),
                request.primary.selection,
                request.primary.selectionArgs?.filterNotNull()?.toTypedArray(),
                buildSortOrderWithLimit(request.primary.sortOrder, request.primary.limit, request.primary.offset)
            ) ?: throw nullCursorError(request.primary.contentUri)

            val joinedRows = mutableListOf<JoinedRow?>()

            primaryCursor.use { pc ->
                while (pc.moveToNext()) {
                    val primaryRow = cursorRowToMap(pc)
                    val relatedData = mutableMapOf<String?, List<Map<String?, Any?>?>?>()

                    for (joinSpec in request.joins) {
                        if (joinSpec == null) continue

                        val parentKeyValue = primaryRow[joinSpec.parentKeyColumn]?.toString() ?: continue
                        relatedData[joinSpec.name] = queryJoinRows(joinSpec, parentKeyValue)
                    }

                    joinedRows.add(JoinedRow(
                        data = primaryRow,
                        related = relatedData
                    ))
                }
            }

            JoinQueryResponse(
                rows = joinedRows,
                rowCount = joinedRows.size.toLong()
            )
        }

    /**
     * Purpose: Read the related rows of one join for one parent row.
     *
     * @param joinSpec the join whose provider is queried.
     * @param parentKeyValue the parent row's key the join's foreign key must equal.
     * @return the related rows; an empty list when the query matched none.
     * @throws QueryFlutterError carrying the join's `contentUri` and name when
     *   the join query or the read of its rows fails, or the query returns
     *   a null cursor, so Dart names the failing provider rather than the
     *   primary one.
     */
    private fun queryJoinRows(joinSpec: JoinSpec, parentKeyValue: String): List<Map<String?, Any?>?> {
        val joinSelection = buildJoinSelection(
            joinSpec.selection,
            joinSpec.foreignKeyColumn,
            parentKeyValue
        )

        // Combine existing args with the parent key value
        val joinArgs = (joinSpec.selectionArgs?.filterNotNull() ?: emptyList()) + parentKeyValue

        try {
            val joinCursor = contentResolver.query(
                Uri.parse(joinSpec.contentUri),
                joinSpec.projection?.filterNotNull()?.toTypedArray(),
                joinSelection,
                joinArgs.toTypedArray(),
                joinSpec.sortOrder
            ) ?: throw nullCursorError(joinSpec.contentUri, joinName = joinSpec.name)

            val relatedRows = mutableListOf<Map<String?, Any?>?>()
            joinCursor.use { jc ->
                while (jc.moveToNext()) {
                    relatedRows.add(cursorRowToMap(jc))
                }
            }
            return relatedRows
        } catch (e: Throwable) {
            // A QueryFlutterError (the null cursor above) passes through as is.
            throw HostErrors.toFlutterError(
                e,
                mapOf("contentUri" to joinSpec.contentUri, "join" to joinSpec.name),
            )
        }
    }

    /**
     * The error both read paths raise when `ContentResolver.query(...)`
     * returns a null cursor (UNFY-284): the provider could not be reached
     * (unknown authority, or its process died during the call) or chose to
     * return none. Reporting it as zero rows would hide that, so it crosses
     * Pigeon as a `PlatformException` with code [HostErrors.UNAVAILABLE],
     * which the Dart side maps to `SimpleQueryError(unavailable)` per
     * `docs/ERROR_MAPPING.md`. [joinName] names the join whose related
     * query failed, if it was not the primary query.
     */
    private fun nullCursorError(contentUri: String, joinName: String? = null): QueryFlutterError {
        val target = if (joinName == null) contentUri else "join '$joinName' ($contentUri)"
        val details = mutableMapOf("reason" to "nullCursor", "contentUri" to contentUri)
        if (joinName != null) details["join"] = joinName
        return QueryFlutterError(
            HostErrors.UNAVAILABLE,
            "simple_query: content provider returned no cursor for $target; " +
                "it is unreachable or returned none",
            details,
        )
    }

    private fun buildSortOrderWithLimit(sortOrder: String?, limit: Long?, offset: Long?): String? {
        // Some ContentProviders support LIMIT in sortOrder
        return when {
            limit != null && offset != null -> "$sortOrder LIMIT $limit OFFSET $offset"
            limit != null -> "${sortOrder ?: ""} LIMIT $limit".trim()
            else -> sortOrder
        }
    }

    private fun buildJoinSelection(
        existingSelection: String?,
        foreignKeyColumn: String,
        parentKeyValue: String
    ): String {
        val fkCondition = "$foreignKeyColumn = ?"
        return if (existingSelection.isNullOrBlank()) {
            fkCondition
        } else {
            "($existingSelection) AND $fkCondition"
        }
    }

    // -------------------------------------------------------------------------
    // Insert
    // -------------------------------------------------------------------------

    override fun insert(request: InsertRequest, callback: (Result<String?>) -> Unit) =
        launchHostCall(callback) {
            val uri = Uri.parse(request.contentUri)
            val values = toContentValues(request.values)
            withProvider(uri) { it.insert(uri, values) }?.toString()
        }

    override fun bulkInsert(request: BulkInsertRequest, callback: (Result<Long>) -> Unit) =
        launchHostCall(callback) {
            val uri = Uri.parse(request.contentUri)
            val valuesArray = request.valuesList
                .filterNotNull()
                .map { toContentValues(it) }
                .toTypedArray()

            withProvider(uri) { it.bulkInsert(uri, valuesArray) }.toLong()
        }

    // -------------------------------------------------------------------------
    // Update
    // -------------------------------------------------------------------------

    override fun update(request: UpdateRequest, callback: (Result<Long>) -> Unit) =
        launchHostCall(callback) {
            val uri = Uri.parse(request.contentUri)
            val values = toContentValues(request.values)

            withProvider(uri) {
                it.update(
                    uri,
                    values,
                    request.selection,
                    request.selectionArgs?.filterNotNull()?.toTypedArray()
                )
            }.toLong()
        }

    // -------------------------------------------------------------------------
    // Delete
    // -------------------------------------------------------------------------

    override fun delete(request: DeleteRequest, callback: (Result<Long>) -> Unit) =
        launchHostCall(callback) {
            val uri = Uri.parse(request.contentUri)

            withProvider(uri) {
                it.delete(
                    uri,
                    request.selection,
                    request.selectionArgs?.filterNotNull()?.toTypedArray()
                )
            }.toLong()
        }

    // -------------------------------------------------------------------------
    // Batch
    // -------------------------------------------------------------------------

    override fun applyBatch(request: BatchRequest, callback: (Result<List<BatchOperationResult?>>) -> Unit) =
        launchHostCall(callback) {
            val operations = request.operations
                .filterNotNull()
                .map { it.toContentProviderOperation() }

            val results = withProvider(request.authority) {
                it.applyBatch(request.authority, ArrayList(operations))
            }
            results.map { result ->
                BatchOperationResult(
                    uri = result?.uri?.toString(),
                    count = result?.count?.toLong()
                )
            }
        }

    private fun BatchOperation.toContentProviderOperation(): android.content.ContentProviderOperation {
        val uri = Uri.parse(contentUri)
        val builder = when (type) {
            BatchOperationType.INSERT ->
                android.content.ContentProviderOperation.newInsert(uri)
            BatchOperationType.UPDATE ->
                android.content.ContentProviderOperation.newUpdate(uri)
            BatchOperationType.DELETE ->
                android.content.ContentProviderOperation.newDelete(uri)
            BatchOperationType.ASSERT_QUERY ->
                android.content.ContentProviderOperation.newAssertQuery(uri)
        }

        values?.filterValues { it != null }?.forEach { (key, value) ->
            if (key != null) {
                when (value) {
                    is String -> builder.withValue(key, value)
                    is Long -> builder.withValue(key, value)
                    is Int -> builder.withValue(key, value)
                    is Double -> builder.withValue(key, value)
                    is Boolean -> builder.withValue(key, value)
                    null -> builder.withValue(key, null)
                    else -> builder.withValue(key, value.toString())
                }
            }
        }

        if (!selection.isNullOrBlank()) {
            builder.withSelection(selection, selectionArgs?.filterNotNull()?.toTypedArray())
        }

        if (type == BatchOperationType.ASSERT_QUERY && expectedCount != null) {
            builder.withExpectedCount(expectedCount.toInt())
        }

        backReferenceIndex?.let {
            val backRefColumn = backReferenceColumn ?: "_id"
            builder.withValueBackReference(backRefColumn, it.toInt())
        }

        return builder.build()
    }

    // -------------------------------------------------------------------------
    // Streaming
    // -------------------------------------------------------------------------

    override fun openStream(contentUri: String, callback: (Result<StreamDescriptor>) -> Unit) =
        launchHostCall(callback) { streamManager.openStream(contentUri) }

    override fun extractToFile(contentUri: String, callback: (Result<String?>) -> Unit) =
        launchHostCall(callback) { streamManager.extractToFile(contentUri) }

    override fun closeStream(streamId: String, callback: (Result<Unit>) -> Unit) =
        launchHostCall(callback) { streamManager.closeStream(streamId) }

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------

    override fun registerObserver(request: ObserverRequest, callback: (Result<String>) -> Unit) =
        launchHostCall(callback) {
            observerRegistry.register(
                contentUri = request.contentUri,
                notifyForDescendants = request.notifyForDescendants
            )
        }

    override fun unregisterObserver(observerId: String, callback: (Result<Unit>) -> Unit) =
        launchHostCall(callback) { observerRegistry.unregister(observerId) }

    override fun unregisterAllObservers(callback: (Result<Unit>) -> Unit) =
        launchHostCall(callback) { observerRegistry.unregisterAll() }

    // Non-callback version for internal use (plugin dispose)
    fun unregisterAllObservers() {
        observerRegistry.unregisterAll()
    }

    // -------------------------------------------------------------------------
    // Metadata
    // -------------------------------------------------------------------------

    override fun getType(contentUri: String, callback: (Result<TypeResponse>) -> Unit) =
        launchHostCall(callback) {
            TypeResponse(mimeType = contentResolver.getType(Uri.parse(contentUri)))
        }

    override fun canonicalize(contentUri: String, callback: (Result<String?>) -> Unit) =
        launchHostCall(callback) {
            contentResolver.canonicalize(Uri.parse(contentUri))?.toString()
        }

    override fun uncanonicalize(contentUri: String, callback: (Result<String?>) -> Unit) =
        launchHostCall(callback) {
            contentResolver.uncanonicalize(Uri.parse(contentUri))?.toString()
        }

    override fun call(request: ProviderCallRequest, callback: (Result<ProviderCallResponse>) -> Unit) =
        launchHostCall(callback) {
            val extras = request.extras?.let { map ->
                android.os.Bundle().apply {
                    map.forEach { (key, value) ->
                        when (value) {
                            is String -> putString(key, value)
                            is Long -> putLong(key, value)
                            is Int -> putInt(key, value)
                            is Double -> putDouble(key, value)
                            is Boolean -> putBoolean(key, value)
                        }
                    }
                }
            }

            val result = withProvider(request.authority) {
                it.call(request.authority, request.method, request.arg, extras)
            }

            val resultMap = mutableMapOf<String?, Any?>()
            result?.keySet()?.forEach { key ->
                resultMap[key] = result.get(key)
            }

            ProviderCallResponse(result = resultMap)
        }

    // -------------------------------------------------------------------------
    // Permissions
    // -------------------------------------------------------------------------

    /**
     * Returns true when the calling process holds [permissionName].
     *
     * Wraps `Context.checkSelfPermission`. Synchronous on the native side; the
     * pigeon contract makes it async on the Dart side. simple_query never
     * *requests* permissions — that is the developer's responsibility (see
     * MEMORY.md). This call only reports current grant state.
     */
    override fun hasPermission(permissionName: String, callback: (Result<Boolean>) -> Unit) {
        try {
            val granted = context.checkSelfPermission(permissionName) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
            callback(Result.success(granted))
        } catch (e: Throwable) {
            callback(Result.failure(HostErrors.toFlutterError(e)))
        }
    }

    /**
     * Returns `Build.VERSION.SDK_INT` so the Dart catalog can branch between
     * legacy (`READ_EXTERNAL_STORAGE`, API ≤32) and modern (`READ_MEDIA_*`,
     * API 33+) media permissions.
     */
    override fun getAndroidSdkInt(callback: (Result<Long>) -> Unit) {
        callback(Result.success(android.os.Build.VERSION.SDK_INT.toLong()))
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /**
     * Purpose: Run one host call on the IO scope and reply to Dart exactly
     * once: with its result, or with its [HostErrors] classification for any
     * failure, so no failure skips the reply or reaches Dart as a Java class
     * name (UNFY-300).
     *
     * @param callback the Pigeon reply callback of the call.
     * @param body the blocking work; its return value is the reply.
     * @return Nothing; the reply is posted to the main looper.
     * @throws CancellationException when [dispose] cancelled the scope: the
     *   engine is detached, so no reply is sent. A `CancellationException`
     *   the blocking [body] raises is a failure like any other.
     */
    private fun <T> launchHostCall(callback: (Result<T>) -> Unit, body: () -> T) {
        scope.launch {
            val result = try {
                Result.success(body())
            } catch (e: Throwable) {
                Result.failure(HostErrors.toFlutterError(e))
            }
            ensureActive()
            mainHandler.post { callback(result) }
        }
    }

    /**
     * Purpose: Run a write or provider call against the provider behind
     * [uri], failing as [HostErrors.UNAVAILABLE] when none is installed, the
     * same as a null cursor on the read paths. `ContentResolver`'s own write
     * methods instead throw `IllegalArgumentException("Unknown URL")`, which
     * would read as a malformed request.
     *
     * @param uri the content URI whose provider runs [operation].
     * @param operation the provider work, given an unstable client.
     * @return [operation]'s result.
     * @throws QueryFlutterError with [HostErrors.UNAVAILABLE] when no
     *   provider serves [uri]; whatever [operation] throws, after the client
     *   is released.
     */
    private fun <T> withProvider(uri: Uri, operation: (ContentProviderClient) -> T): T {
        val client = contentResolver.acquireUnstableContentProviderClient(uri)
            ?: throw noProviderError("contentUri", uri.toString())
        return client.use(operation)
    }

    /**
     * Purpose: Run a batch or provider call against the provider registered
     * for [authority], failing as [HostErrors.UNAVAILABLE] when none is.
     *
     * @param authority the provider authority.
     * @param operation the provider work, given an unstable client.
     * @return [operation]'s result.
     * @throws QueryFlutterError with [HostErrors.UNAVAILABLE] when no
     *   provider serves [authority]; whatever [operation] throws, after the
     *   client is released.
     */
    private fun <T> withProvider(authority: String, operation: (ContentProviderClient) -> T): T {
        val client = contentResolver.acquireUnstableContentProviderClient(authority)
            ?: throw noProviderError("authority", authority)
        return client.use(operation)
    }

    /**
     * Purpose: Build the `unavailable` error for a provider that cannot be
     * acquired.
     *
     * @param key the details key naming [target] (`contentUri` or `authority`).
     * @param target the URI or authority no provider serves.
     * @return the error, with details `{reason: noProvider, <key>: target}`.
     * @throws Nothing.
     */
    private fun noProviderError(key: String, target: String) = QueryFlutterError(
        HostErrors.UNAVAILABLE,
        "simple_query: no content provider for $target",
        mapOf("reason" to "noProvider", key to target),
    )

    private fun cursorRowToMap(cursor: Cursor): Map<String?, Any?> {
        val map = mutableMapOf<String?, Any?>()
        for (i in 0 until cursor.columnCount) {
            val columnName = cursor.getColumnName(i)
            val value: Any? = when (cursor.getType(i)) {
                Cursor.FIELD_TYPE_NULL -> null
                Cursor.FIELD_TYPE_INTEGER -> cursor.getLong(i)
                Cursor.FIELD_TYPE_FLOAT -> cursor.getDouble(i)
                Cursor.FIELD_TYPE_STRING -> cursor.getString(i)
                Cursor.FIELD_TYPE_BLOB -> {
                    // Never send BLOB over method channel!
                    // Return null and let caller use openStream()
                    null
                }
                else -> cursor.getString(i)
            }
            map[columnName] = value
        }
        return map
    }

    private fun toContentValues(map: Map<String?, Any?>?): ContentValues {
        val values = ContentValues()
        map?.forEach { (key, value) ->
            if (key == null) return@forEach
            when (value) {
                null -> values.putNull(key)
                is String -> values.put(key, value)
                is Long -> values.put(key, value)
                is Int -> values.put(key, value)
                is Double -> values.put(key, value)
                is Boolean -> values.put(key, if (value) 1 else 0)
                is ByteArray -> values.put(key, value)
                else -> values.put(key, value.toString())
            }
        }
        return values
    }

    /**
     * Clean up all resources on plugin detach. Cancelling the scope first
     * means no host call replies to the detached engine.
     */
    fun dispose() {
        scope.cancel()
        observerRegistry.unregisterAll()
        streamManager.closeAll()
    }
}
