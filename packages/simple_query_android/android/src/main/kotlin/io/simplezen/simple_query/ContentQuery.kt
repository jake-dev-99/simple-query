package io.simplezen.simple_query

import android.content.Context
import android.database.Cursor
import android.net.Uri

/**
 * Native-side helper sibling plugins can call when their own
 * Kotlin code needs to read rows out of a `content://` provider.
 *
 * ### Why this exists
 *
 * Rule 1 of the cross-plugin consolidation is *"all
 * content-provider queries route through simple_query"*. That's
 * easy to uphold at the **Dart API boundary** — call
 * `SimpleQueryNative.instance.query(...)` from Dart and you're
 * using the canonical path. But plugins with internal Kotlin
 * helpers (e.g. `simple-sms`'s `MmsDatabaseWriter` building an
 * outgoing MMS row-by-row) have typically dropped straight to
 * `context.contentResolver.query(...)` because there was no
 * Kotlin API to delegate to.
 *
 * [ContentQuery] is that API: a thin, allocation-conscious
 * wrapper over `ContentResolver.query(...)` + cursor draining
 * that returns a list of `(columnName -> value)` row maps —
 * exactly the shape [QueryHostApiImpl] returns to Dart via
 * Pigeon.
 *
 * ### Gradle wiring for consumers
 *
 * Two-line setup in any consuming plugin (cross-repo is fine):
 *
 * 1. Consuming plugin's root `pubspec.yaml`:
 *    ```yaml
 *    dependencies:
 *      simple_query: ^0.3.0
 *    ```
 * 2. Consuming plugin's `android/build.gradle[.kts]`:
 *    ```groovy
 *    dependencies {
 *      implementation(project(":simple_query_android"))
 *    }
 *    ```
 *
 * Flutter's plugin-loader walks the pubspec graph at app-build
 * time and synthesises a Gradle project entry for
 * `:simple_query_android` alongside the consuming plugin. No
 * composite-build or Maven publication needed.
 *
 * The same federated-override gotcha documented on
 * `simple_permissions_android.PermissionGuards` applies here —
 * if the consuming plugin's example app path-overrides
 * `simple_query`, it must also override `simple_query_android`
 * (and `simple_query_platform_interface` if used) so the local
 * workspace version wins against the pub.dev cache.
 *
 * ### BLOB handling
 *
 * The Pigeon `query()` path drops BLOB columns to null to avoid
 * shipping bytes over the method channel. [ContentQuery] stays
 * in-process (Kotlin-to-Kotlin), so it *could* return raw byte
 * arrays — but it matches the Pigeon path's null-coalescing
 * behaviour so callers can swap between the two without
 * changing downstream logic. If a caller needs BLOB bytes,
 * drop to `ContentResolver.openInputStream(uri)` directly
 * (there's no simple-query wrapper for stream IO).
 */
object ContentQuery {

    /**
     * Run a single query against `contentUri` and return the
     * resulting rows as `(columnName -> value)` maps.
     *
     * Mirrors `ContentResolver.query(...)`'s parameter list
     * exactly so callers that already have [projection],
     * [selection], etc. can pass them through unchanged.
     * Returns an empty list when the resolver hands back a null
     * cursor (the standard Android "no access" / "no match"
     * signal).
     *
     * Thread-safe and blocking. Callers that need async should
     * wrap with their own coroutine / executor — matching the
     * primitive it replaces.
     */
    @JvmStatic
    fun query(
        context: Context,
        contentUri: String,
        projection: Array<String>? = null,
        selection: String? = null,
        selectionArgs: Array<String>? = null,
        sortOrder: String? = null,
    ): List<Map<String, Any?>> {
        val cursor = context.contentResolver.query(
            Uri.parse(contentUri),
            projection,
            selection,
            selectionArgs,
            sortOrder,
        ) ?: return emptyList()

        return cursor.use { c -> drainRows(c) }
    }

    /**
     * Collect every row from an already-opened cursor into
     * `(columnName -> value)` maps. Useful when a caller opened
     * the cursor via a non-query primitive (e.g.
     * `ContentResolver.openFile(...)` followed by
     * `ContentResolver.query(...)` with a different URI variant)
     * and still wants the standardised row shape.
     *
     * Does not close the cursor — the caller owns its lifecycle.
     */
    @JvmStatic
    fun drainRows(cursor: Cursor): List<Map<String, Any?>> {
        val rows = mutableListOf<Map<String, Any?>>()
        while (cursor.moveToNext()) {
            rows.add(cursorRowToMap(cursor))
        }
        return rows
    }

    private fun cursorRowToMap(cursor: Cursor): Map<String, Any?> {
        val map = mutableMapOf<String, Any?>()
        for (i in 0 until cursor.columnCount) {
            val columnName = cursor.getColumnName(i) ?: continue
            val value: Any? = when (cursor.getType(i)) {
                Cursor.FIELD_TYPE_NULL -> null
                Cursor.FIELD_TYPE_INTEGER -> cursor.getLong(i)
                Cursor.FIELD_TYPE_FLOAT -> cursor.getDouble(i)
                Cursor.FIELD_TYPE_STRING -> cursor.getString(i)
                // Match the Pigeon path: never ship BLOB bytes over the
                // method channel. For in-process callers that truly need
                // bytes, drop to ContentResolver.openInputStream(uri).
                Cursor.FIELD_TYPE_BLOB -> null
                else -> cursor.getString(i)
            }
            map[columnName] = value
        }
        return map
    }
}
