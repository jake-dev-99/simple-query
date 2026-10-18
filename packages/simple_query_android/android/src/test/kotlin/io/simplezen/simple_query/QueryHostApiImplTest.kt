package io.simplezen.simple_query

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.Looper
import android.os.ParcelFileDescriptor
import io.flutter.plugin.common.BinaryMessenger
import java.nio.ByteBuffer
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.Dispatchers
import org.junit.After
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/**
 * Drives [QueryHostApiImpl] through the generated Pigeon channels and
 * asserts the reply envelope Dart receives: a null resolver cursor must
 * arrive as an `unavailable` error, not as zero rows (UNFY-284), and a
 * provider exception as its vocabulary code, not its Java class name
 * (UNFY-300).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class QueryHostApiImplTest {
    // Answers by URI path: "null" -> null cursor, "empty" -> zero rows,
    // "rows" -> two rows (one BLOB column, which the Pigeon path drops).
    class FakeProvider : ContentProvider() {
        val queried = mutableListOf<String?>()
        val sortOrders = mutableListOf<String?>()
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
            sortOrders.add(sortOrder)
            val columns = arrayOf("_id", "body", "data")
            val cursor = when (uri.lastPathSegment) {
                "denied" -> throw SecurityException("Permission Denial: reading $uri")
                "broken" -> throw IllegalStateException("provider state")
                "fatal" -> throw AssertionError("provider invariant")
                "cancelled" -> throw java.util.concurrent.CancellationException("future cancelled")
                "badrows" -> object : MatrixCursor(columns) {
                    init { addRow(arrayOf<Any?>(1L, "x", null)) }
                    override fun getType(column: Int): Int = throw IllegalStateException("window")
                }
                "null" -> return null
                "empty" -> MatrixCursor(columns)
                "rows" -> MatrixCursor(columns).apply {
                    addRow(arrayOf<Any?>(1L, "hello", byteArrayOf(1, 2)))
                    addRow(arrayOf<Any?>(2L, "world", null))
                }
                else -> error("unexpected uri $uri")
            }
            opened.add(cursor)
            return cursor
        }

        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? =
            throw SecurityException("Permission Denial: opening $uri")

        override fun getType(uri: Uri): String? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? =
            throw UnsupportedOperationException("read-only provider")
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?) = 0
        override fun update(
            uri: Uri,
            values: ContentValues?,
            selection: String?,
            selectionArgs: Array<String>?,
        ) = 0
    }

    private val handlers = mutableMapOf<String, BinaryMessenger.BinaryMessageHandler>()
    private val messenger = object : BinaryMessenger {
        override fun send(channel: String, message: ByteBuffer?) = Unit

        override fun send(
            channel: String,
            message: ByteBuffer?,
            callback: BinaryMessenger.BinaryReply?,
        ) = Unit

        override fun setMessageHandler(
            channel: String,
            handler: BinaryMessenger.BinaryMessageHandler?,
        ) {
            if (handler == null) handlers.remove(channel) else handlers[channel] = handler
        }
    }

    private lateinit var provider: FakeProvider
    private lateinit var api: QueryHostApiImpl

    @Before
    fun setUp() {
        val context = RuntimeEnvironment.getApplication()
        provider = Robolectric.buildContentProvider(FakeProvider::class.java)
            .create(AUTHORITY)
            .get()
        api = QueryHostApiImpl(context, QueryFlutterApi(messenger), Dispatchers.Unconfined)
        QueryHostApi.setUp(messenger, api)
    }

    @After
    fun tearDown() {
        QueryHostApi.setUp(messenger, null)
        api.dispose()
    }

    @Test
    fun queryNullCursorRepliesUnavailable() {
        val reply = call("query", QueryRequest(contentUri = uri("null")))

        assertUnavailable(reply, mapOf("reason" to "nullCursor", "contentUri" to uri("null")))
        assertEquals(listOf<String?>("null"), provider.queried)
    }

    @Test
    fun queryEmptyCursorRepliesZeroRows() {
        val reply = call("query", QueryRequest(contentUri = uri("empty")))

        val response = reply.single() as QueryResponse
        assertEquals(emptyList(), response.rows)
        assertEquals(0L, response.rowCount)
        assertEquals(listOf<String?>("_id", "body", "data"), response.columnNames)
        assertTrue(provider.opened.single().isClosed)
    }

    @Test
    fun queryRowCursorRepliesRows() {
        val reply = call("query", QueryRequest(contentUri = uri("rows")))

        val response = reply.single() as QueryResponse
        assertEquals(
            listOf<Any?>(
                mapOf<Any?, Any?>("_id" to 1L, "body" to "hello", "data" to null),
                mapOf<Any?, Any?>("_id" to 2L, "body" to "world", "data" to null),
            ),
            response.rows,
        )
        assertEquals(2L, response.rowCount)
        assertTrue(provider.opened.single().isClosed)
    }

    @Test
    fun joinPrimaryNullCursorRepliesUnavailable() {
        val reply = call("queryWithJoins", joinRequest(primary = "null", join = "rows"))

        assertUnavailable(reply, mapOf("reason" to "nullCursor", "contentUri" to uri("null")))
        assertEquals(listOf<String?>("null"), provider.queried)
    }

    @Test
    fun joinRelatedNullCursorRepliesUnavailableAndClosesPrimary() {
        val reply = call("queryWithJoins", joinRequest(primary = "rows", join = "null"))

        assertUnavailable(
            reply,
            mapOf("reason" to "nullCursor", "contentUri" to uri("null"), "join" to "parts"),
        )
        assertTrue((reply[1] as String).contains("join 'parts'"))
        assertEquals(listOf<String?>("rows", "null"), provider.queried)
        assertTrue(provider.opened.single().isClosed)
    }

    @Test
    fun joinEmptyRelatedCursorRepliesEmptyRelatedRows() {
        val reply = call("queryWithJoins", joinRequest(primary = "rows", join = "empty"))

        val response = reply.single() as JoinQueryResponse
        assertEquals(2L, response.rowCount)
        assertEquals(
            listOf<Any?>(mapOf<Any?, Any?>("parts" to emptyList<Any?>())),
            response.rows.map { it!!.related }.distinct(),
        )
        assertEquals(3, provider.opened.size)
        assertTrue(provider.opened.all { it.isClosed })
    }

    @Test
    fun queryProviderSecurityExceptionRepliesPermissionDenied() {
        val reply = call("query", QueryRequest(contentUri = uri("denied")))

        assertHostError(reply, HostErrors.PERMISSION_DENIED, "java.lang.SecurityException")
        assertTrue((reply[1] as String).contains("Permission Denial: reading ${uri("denied")}"))
    }

    @Test
    fun queryUnlistedFailureRepliesTransient() {
        val reply = call("query", QueryRequest(contentUri = uri("broken")))

        assertHostError(reply, HostErrors.TRANSIENT, "java.lang.IllegalStateException")
    }

    @Test
    fun queryErrorStillRepliesInsteadOfEscaping() {
        val reply = call("query", QueryRequest(contentUri = uri("fatal")))

        assertHostError(reply, HostErrors.TRANSIENT, "java.lang.AssertionError")
    }

    @Test
    fun bodyCancellationExceptionStillReplies() {
        val reply = call("query", QueryRequest(contentUri = uri("cancelled")))

        assertHostError(reply, HostErrors.TRANSIENT, "java.util.concurrent.CancellationException")
    }

    @Test
    fun joinRowReadFailureNamesTheJoinProvider() {
        val reply = call("queryWithJoins", joinRequest(primary = "rows", join = "badrows"))

        assertHostError(reply, HostErrors.TRANSIENT, "java.lang.IllegalStateException")
        val details = reply[2] as Map<*, *>
        assertEquals(uri("badrows"), details["contentUri"])
        assertEquals("parts", details["join"])
    }

    @Test
    fun extractToFileFailureRepliesItsClassificationAndLeavesNoTempFile() {
        val cacheDir = RuntimeEnvironment.getApplication().cacheDir
        val before = cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0

        val reply = call("extractToFile", uri("rows"))

        assertHostError(reply, HostErrors.PERMISSION_DENIED, "java.lang.SecurityException")
        assertEquals(before, cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0)
    }

    @Test
    fun joinRelatedFailureNamesTheJoinProvider() {
        val reply = call("queryWithJoins", joinRequest(primary = "rows", join = "denied"))

        assertHostError(reply, HostErrors.PERMISSION_DENIED, "java.lang.SecurityException")
        val details = reply[2] as Map<*, *>
        assertEquals(uri("denied"), details["contentUri"])
        assertEquals("parts", details["join"])
        assertTrue(provider.opened.single().isClosed)
    }

    @Test
    fun joinNullSortWithLimitSendsDefaultSort() {
        call("queryWithJoins", joinRequest(primary = "rows", join = "empty", limit = 10))

        assertEquals("_id ASC LIMIT 10", provider.sortOrders.first())
    }

    @Test
    fun joinNullSortWithLimitAndOffsetSendsDefaultSort() {
        call("queryWithJoins", joinRequest(primary = "rows", join = "empty", limit = 10, offset = 0))

        assertEquals("_id ASC LIMIT 10 OFFSET 0", provider.sortOrders.first())
    }

    @Test
    fun joinBlankSortWithLimitSendsDefaultSort() {
        call("queryWithJoins", joinRequest(primary = "rows", join = "empty", sortOrder = "  ", limit = 10))

        assertEquals("_id ASC LIMIT 10", provider.sortOrders.first())
    }

    @Test
    fun joinGivenSortWithLimitKeepsSort() {
        call(
            "queryWithJoins",
            joinRequest(primary = "rows", join = "empty", sortOrder = "date DESC", limit = 5, offset = 2),
        )

        assertEquals("date DESC LIMIT 5 OFFSET 2", provider.sortOrders.first())
    }

    @Test
    fun queryNullSortWithLimitAndOffsetSendsDefaultSort() {
        call("query", QueryRequest(contentUri = uri("rows"), limit = 10, offset = 0))

        assertEquals("_id ASC LIMIT 10 OFFSET 0", provider.sortOrders.first())
    }

    @Test
    fun queryNullSortWithoutLimitSendsNoSort() {
        call("query", QueryRequest(contentUri = uri("rows")))

        assertNull(provider.sortOrders.first())
    }

    @Test
    fun insertUnsupportedRepliesNotSupported() {
        val reply = call("insert", InsertRequest(contentUri = uri("rows"), values = mapOf("body" to "x")))

        assertHostError(reply, HostErrors.NOT_SUPPORTED, "java.lang.UnsupportedOperationException")
    }

    @Test
    fun writeToMissingProviderRepliesUnavailable() {
        val missing = "content://io.simplezen.simple_query.missing/items"
        val reply = call("insert", InsertRequest(contentUri = missing, values = mapOf("body" to "x")))

        assertEquals(HostErrors.UNAVAILABLE, reply[0])
        assertEquals("simple_query: no content provider for $missing", reply[1])
        assertEquals(mapOf("reason" to "noProvider", "contentUri" to missing), reply[2])
    }

    @Test
    fun batchForMissingAuthorityRepliesUnavailable() {
        val authority = "io.simplezen.simple_query.missing"
        val reply = call("applyBatch", BatchRequest(authority = authority, operations = emptyList()))

        assertEquals(HostErrors.UNAVAILABLE, reply[0])
        assertEquals(mapOf("reason" to "noProvider", "authority" to authority), reply[2])
    }

    @Test
    fun noReplyIsSentAfterDispose() {
        api.dispose()

        assertNull(send("query", QueryRequest(contentUri = uri("rows"))))
        assertTrue(provider.queried.isEmpty())
    }

    private fun joinRequest(
        primary: String,
        join: String,
        sortOrder: String? = null,
        limit: Long? = null,
        offset: Long? = null,
    ) = JoinQueryRequest(
        primary = QueryRequest(
            contentUri = uri(primary),
            sortOrder = sortOrder,
            limit = limit,
            offset = offset,
        ),
        joins = listOf(
            JoinSpec(
                name = "parts",
                contentUri = uri(join),
                foreignKeyColumn = "mid",
                parentKeyColumn = "_id",
            ),
        ),
    )

    /** Sends one Pigeon message and returns the decoded reply envelope. */
    private fun call(method: String, request: Any): List<Any?> {
        val bytes = checkNotNull(send(method, request)) { "no reply for $method" }
        bytes.flip()
        @Suppress("UNCHECKED_CAST")
        return QueryHostApi.codec.decodeMessage(bytes) as List<Any?>
    }

    /** Sends one Pigeon message and returns the raw reply, or null when none came. */
    private fun send(method: String, request: Any): ByteBuffer? {
        val handler = handlers.getValue("dev.flutter.pigeon.simple_query.QueryHostApi.$method")
        val message = QueryHostApi.codec.encodeMessage(listOf(request))!!
        message.flip()
        var reply: ByteBuffer? = null
        handler.onMessage(message) { reply = it }

        // The read runs inline (Unconfined); the reply is posted to the main looper.
        shadowOf(Looper.getMainLooper()).idle()
        return reply
    }

    /** A Pigeon error reply is `[code, message, details]`. */
    private fun assertUnavailable(reply: List<Any?>, details: Map<String, String>) {
        assertEquals(3, reply.size)
        assertEquals("unavailable", reply[0])
        assertTrue((reply[1] as String).startsWith("simple_query: "))
        assertEquals(details, reply[2])
    }

    /** A classified failure carries its vocabulary code and native type (UNFY-300). */
    private fun assertHostError(reply: List<Any?>, code: String, nativeType: String) {
        assertEquals(3, reply.size)
        assertEquals(code, reply[0])
        assertTrue((reply[1] as String).startsWith("simple_query: "))
        assertEquals(nativeType, (reply[2] as Map<*, *>)["nativeType"])
    }

    private fun uri(path: String) = "content://$AUTHORITY/$path"

    private companion object {
        const val AUTHORITY = "io.simplezen.simple_query.hostapi.test"
    }
}
