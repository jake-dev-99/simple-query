package io.simplezen.simple_query

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/**
 * Unit tests for the null-cursor vs zero-rows contract of
 * [ContentQuery.query] and [ContentQuery.queryOrNull] (UNFY-284).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class ContentQueryTest {
    // Answers by URI path: "null" -> null cursor, "empty" -> zero rows,
    // "rows" -> two rows covering every cursor field type.
    class FakeProvider : ContentProvider() {
        val queried = mutableListOf<String?>()
        val opened = mutableListOf<Cursor>()

        override fun onCreate() = true

        override fun query(
            uri: Uri,
            projection: Array<String>?,
            selection: String?,
            selectionArgs: Array<String>?,
            sortOrder: String?,
        ): Cursor? {
            queried.add(uri.lastPathSegment)
            val columns = arrayOf("_id", "body", "rate", "note", "data")
            val cursor = when (uri.lastPathSegment) {
                "null" -> return null
                "empty" -> MatrixCursor(columns)
                "rows" -> MatrixCursor(columns).apply {
                    addRow(arrayOf<Any?>(1L, "hello", 1.5, null, byteArrayOf(1, 2)))
                    addRow(arrayOf<Any?>(2L, "world", 2.5, "n", null))
                }
                else -> error("unexpected uri $uri")
            }
            opened.add(cursor)
            return cursor
        }

        override fun getType(uri: Uri): String? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?) = 0
        override fun update(
            uri: Uri,
            values: ContentValues?,
            selection: String?,
            selectionArgs: Array<String>?,
        ) = 0
    }

    private lateinit var context: Context
    private lateinit var provider: FakeProvider

    @Before
    fun registerProvider() {
        context = RuntimeEnvironment.getApplication()
        provider = Robolectric.buildContentProvider(FakeProvider::class.java)
            .create(AUTHORITY)
            .get()
    }

    @Test
    fun nullCursorIsNullForQueryOrNullAndEmptyForQuery() {
        assertNull(ContentQuery.queryOrNull(context, "content://$AUTHORITY/null"))
        assertEquals(emptyList(), ContentQuery.query(context, "content://$AUTHORITY/null"))
        // The provider itself returned null; an unregistered authority would too.
        assertEquals(listOf<String?>("null", "null"), provider.queried)
    }

    @Test
    fun emptyCursorIsEmptyListForBoth() {
        assertEquals(emptyList(), ContentQuery.queryOrNull(context, "content://$AUTHORITY/empty"))
        assertEquals(emptyList(), ContentQuery.query(context, "content://$AUTHORITY/empty"))
        assertEquals(2, provider.opened.size)
        assertTrue(provider.opened.all { it.isClosed })
    }

    @Test
    fun rowCursorReturnsDrainedRowsForBoth() {
        val expected = listOf(
            mapOf("_id" to 1L, "body" to "hello", "rate" to 1.5, "note" to null, "data" to null),
            mapOf("_id" to 2L, "body" to "world", "rate" to 2.5, "note" to "n", "data" to null),
        )
        assertEquals(expected, ContentQuery.queryOrNull(context, "content://$AUTHORITY/rows"))
        assertEquals(expected, ContentQuery.query(context, "content://$AUTHORITY/rows"))
        assertEquals(2, provider.opened.size)
        assertTrue(provider.opened.all { it.isClosed })
    }

    private companion object {
        const val AUTHORITY = "io.simplezen.simple_query.test"
    }
}
