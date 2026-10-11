package io.simplezen.simple_query

import android.content.OperationApplicationException
import android.database.sqlite.SQLiteConstraintException
import android.database.sqlite.SQLiteDatabaseCorruptException
import android.database.sqlite.SQLiteDatabaseLockedException
import android.database.sqlite.SQLiteDiskIOException
import android.database.sqlite.SQLiteException
import android.database.sqlite.SQLiteFullException
import android.database.sqlite.SQLiteTableLockedException
import android.os.DeadObjectException
import android.os.OperationCanceledException
import android.os.RemoteException
import java.io.FileNotFoundException
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertSame
import kotlin.test.assertTrue
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Pins the host-failure classification table in `docs/ERROR_MAPPING.md`
 * (UNFY-300): each native failure maps to one vocabulary code, subclasses
 * land with their parent, and anything unlisted is `transient`.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class HostErrorsTest {
    @Test
    fun classifiesEveryDocumentedFailure() {
        val table = listOf<Pair<Throwable, String>>(
            SecurityException("Permission Denial") to HostErrors.PERMISSION_DENIED,
            IllegalArgumentException("Unknown URI content://nope") to HostErrors.INVALID_QUERY,
            NumberFormatException("For input string: \"x\"") to HostErrors.INVALID_QUERY,
            UnsupportedOperationException("insert not supported") to HostErrors.NOT_SUPPORTED,
            SQLiteConstraintException("UNIQUE constraint failed") to HostErrors.INVALID_QUERY,
            SQLiteException("no such column: thread") to HostErrors.INVALID_QUERY,
            SQLiteFullException("database or disk is full") to HostErrors.TRANSIENT,
            SQLiteDiskIOException("disk I/O error") to HostErrors.TRANSIENT,
            SQLiteDatabaseLockedException("database is locked") to HostErrors.TRANSIENT,
            SQLiteTableLockedException("database table is locked") to HostErrors.TRANSIENT,
            SQLiteDatabaseCorruptException("file is not a database") to HostErrors.UNAVAILABLE,
            OperationApplicationException("wrong number of rows: 0") to HostErrors.INVALID_QUERY,
            OperationCanceledException("cancelled") to HostErrors.TRANSIENT,
            FileNotFoundException("No content provider: content://x") to HostErrors.UNAVAILABLE,
            RemoteException("binder") to HostErrors.UNAVAILABLE,
            DeadObjectException("provider died") to HostErrors.UNAVAILABLE,
            IllegalStateException("unlisted") to HostErrors.TRANSIENT,
            NullPointerException("unlisted") to HostErrors.TRANSIENT,
        )

        for ((error, code) in table) {
            assertEquals(code, HostErrors.codeFor(error), error.javaClass.name)
        }
    }

    @Test
    fun keepsAnExistingFlutterErrorUnchanged() {
        val nullCursor = QueryFlutterError(HostErrors.UNAVAILABLE, "simple_query: no cursor", null)

        assertSame(nullCursor, HostErrors.toFlutterError(nullCursor))
    }

    @Test
    fun preservesTheNativeFailureInMessageAndDetails() {
        val error = HostErrors.toFlutterError(SecurityException("Permission Denial: reading sms"))

        assertEquals(HostErrors.PERMISSION_DENIED, error.code)
        assertEquals("simple_query: SecurityException: Permission Denial: reading sms", error.message)
        val details = error.details as Map<*, *>
        assertEquals("java.lang.SecurityException", details["nativeType"])
        assertTrue((details["nativeStackTrace"] as String).contains("Permission Denial: reading sms"))
    }

    @Test
    fun namesTheNativeTypeWhenTheFailureHasNoMessage() {
        val error = HostErrors.toFlutterError(IllegalStateException())

        assertEquals(HostErrors.TRANSIENT, error.code)
        assertEquals("simple_query: IllegalStateException", error.message)
    }
}
