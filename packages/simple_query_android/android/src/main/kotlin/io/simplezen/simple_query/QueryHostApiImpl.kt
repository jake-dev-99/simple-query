package io.simplezen.simple_query

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

    override fun query(request: QueryRequest, callback: (Result<QueryResponse>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(request.contentUri)

                val cursor = contentResolver.query(
                    uri,
                    request.projection?.filterNotNull()?.toTypedArray(),
                    request.selection,
                    request.selectionArgs?.filterNotNull()?.toTypedArray(),
                    buildSortOrderWithLimit(request.sortOrder, request.limit, request.offset)
                ) ?: throw nullCursorError(request.contentUri)

                val response = cursor.use { c ->
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

                mainHandler.post { callback(Result.success(response)) }
            } catch (e: QueryFlutterError) {
                // QueryFlutterError extends Throwable, not Exception.
                postFailure(callback, e)
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun queryWithJoins(request: JoinQueryRequest, callback: (Result<JoinQueryResponse>) -> Unit) {
        scope.launch {
            try {
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

                            val joinUri = Uri.parse(joinSpec.contentUri)
                            val joinSelection = buildJoinSelection(
                                joinSpec.selection,
                                joinSpec.foreignKeyColumn,
                                parentKeyValue
                            )

                            // Combine existing args with the parent key value
                            val joinArgs = (joinSpec.selectionArgs?.filterNotNull() ?: emptyList()) + parentKeyValue

                            val joinCursor = contentResolver.query(
                                joinUri,
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

                            relatedData[joinSpec.name] = relatedRows
                        }

                        joinedRows.add(JoinedRow(
                            data = primaryRow,
                            related = relatedData
                        ))
                    }
                }

                val response = JoinQueryResponse(
                    rows = joinedRows,
                    rowCount = joinedRows.size.toLong()
                )

                mainHandler.post { callback(Result.success(response)) }
            } catch (e: QueryFlutterError) {
                // QueryFlutterError extends Throwable, not Exception.
                postFailure(callback, e)
            } catch (e: Exception) {
                postFailure(callback, e)
            }
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

    override fun insert(request: InsertRequest, callback: (Result<String?>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(request.contentUri)
                val values = toContentValues(request.values)
                val resultUri = contentResolver.insert(uri, values)
                mainHandler.post { callback(Result.success(resultUri?.toString())) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun bulkInsert(request: BulkInsertRequest, callback: (Result<Long>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(request.contentUri)
                val valuesArray = request.valuesList
                    .filterNotNull()
                    .map { toContentValues(it) }
                    .toTypedArray()

                val count = contentResolver.bulkInsert(uri, valuesArray).toLong()
                mainHandler.post { callback(Result.success(count)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    // -------------------------------------------------------------------------
    // Update
    // -------------------------------------------------------------------------

    override fun update(request: UpdateRequest, callback: (Result<Long>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(request.contentUri)
                val values = toContentValues(request.values)

                val count = contentResolver.update(
                    uri,
                    values,
                    request.selection,
                    request.selectionArgs?.filterNotNull()?.toTypedArray()
                ).toLong()

                mainHandler.post { callback(Result.success(count)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    // -------------------------------------------------------------------------
    // Delete
    // -------------------------------------------------------------------------

    override fun delete(request: DeleteRequest, callback: (Result<Long>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(request.contentUri)

                val count = contentResolver.delete(
                    uri,
                    request.selection,
                    request.selectionArgs?.filterNotNull()?.toTypedArray()
                ).toLong()

                mainHandler.post { callback(Result.success(count)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    // -------------------------------------------------------------------------
    // Batch
    // -------------------------------------------------------------------------

    override fun applyBatch(request: BatchRequest, callback: (Result<List<BatchOperationResult?>>) -> Unit) {
        scope.launch {
            try {
                val operations = request.operations
                    .filterNotNull()
                    .map { it.toContentProviderOperation() }

                val results = contentResolver.applyBatch(request.authority, ArrayList(operations))
                val batchResults = results.map { result ->
                    BatchOperationResult(
                        uri = result?.uri?.toString(),
                        count = result?.count?.toLong()
                    )
                }

                mainHandler.post { callback(Result.success(batchResults)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
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

    override fun openStream(contentUri: String, callback: (Result<StreamDescriptor>) -> Unit) {
        scope.launch {
            try {
                val descriptor = streamManager.openStream(contentUri)
                mainHandler.post { callback(Result.success(descriptor)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun extractToFile(contentUri: String, callback: (Result<String?>) -> Unit) {
        scope.launch {
            try {
                val filePath = streamManager.extractToFile(contentUri)
                mainHandler.post { callback(Result.success(filePath)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun closeStream(streamId: String, callback: (Result<Unit>) -> Unit) {
        scope.launch {
            try {
                streamManager.closeStream(streamId)
                mainHandler.post { callback(Result.success(Unit)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------

    override fun registerObserver(request: ObserverRequest, callback: (Result<String>) -> Unit) {
        scope.launch {
            try {
                val observerId = observerRegistry.register(
                    contentUri = request.contentUri,
                    notifyForDescendants = request.notifyForDescendants
                )
                mainHandler.post { callback(Result.success(observerId)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun unregisterObserver(observerId: String, callback: (Result<Unit>) -> Unit) {
        scope.launch {
            try {
                observerRegistry.unregister(observerId)
                mainHandler.post { callback(Result.success(Unit)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun unregisterAllObservers(callback: (Result<Unit>) -> Unit) {
        scope.launch {
            try {
                observerRegistry.unregisterAll()
                mainHandler.post { callback(Result.success(Unit)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    // Non-callback version for internal use (plugin dispose)
    fun unregisterAllObservers() {
        observerRegistry.unregisterAll()
    }

    // -------------------------------------------------------------------------
    // Metadata
    // -------------------------------------------------------------------------

    override fun getType(contentUri: String, callback: (Result<TypeResponse>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(contentUri)
                val mimeType = contentResolver.getType(uri)
                mainHandler.post { callback(Result.success(TypeResponse(mimeType = mimeType))) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun canonicalize(contentUri: String, callback: (Result<String?>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(contentUri)
                val result = contentResolver.canonicalize(uri)?.toString()
                mainHandler.post { callback(Result.success(result)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun uncanonicalize(contentUri: String, callback: (Result<String?>) -> Unit) {
        scope.launch {
            try {
                val uri = Uri.parse(contentUri)
                val result = contentResolver.uncanonicalize(uri)?.toString()
                mainHandler.post { callback(Result.success(result)) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
    }

    override fun call(request: ProviderCallRequest, callback: (Result<ProviderCallResponse>) -> Unit) {
        scope.launch {
            try {
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

                val result = contentResolver.call(
                    request.authority,
                    request.method,
                    request.arg,
                    extras
                )

                val resultMap = mutableMapOf<String?, Any?>()
                result?.keySet()?.forEach { key ->
                    resultMap[key] = result.get(key)
                }

                mainHandler.post { callback(Result.success(ProviderCallResponse(result = resultMap))) }
            } catch (e: Exception) {
                postFailure(callback, e)
            }
        }
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
        } catch (e: Exception) {
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
     * Purpose: Reply to a failed host call on the main thread with its
     * [HostErrors] classification, so no Java class name reaches Dart as an
     * error code.
     *
     * @param callback the Pigeon reply callback of the failed call.
     * @param error the failure the call raised.
     * @return Nothing; the reply is posted to the main looper.
     * @throws Nothing.
     */
    private fun <T> postFailure(callback: (Result<T>) -> Unit, error: Throwable) {
        val hostError = HostErrors.toFlutterError(error)
        mainHandler.post { callback(Result.failure(hostError)) }
    }

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
     * Clean up all resources on plugin detach.
     */
    fun dispose() {
        observerRegistry.unregisterAll()
        streamManager.closeAll()
    }
}
