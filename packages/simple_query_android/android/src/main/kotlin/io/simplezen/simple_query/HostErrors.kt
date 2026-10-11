package io.simplezen.simple_query

import android.content.OperationApplicationException
import android.database.sqlite.SQLiteConstraintException
import android.database.sqlite.SQLiteDatabaseCorruptException
import android.database.sqlite.SQLiteDatabaseLockedException
import android.database.sqlite.SQLiteDiskIOException
import android.database.sqlite.SQLiteException
import android.database.sqlite.SQLiteFullException
import android.database.sqlite.SQLiteTableLockedException
import android.os.OperationCanceledException
import android.os.RemoteException
import android.util.Log
import java.io.FileNotFoundException

/**
 * Purpose: Classify every host failure into the error-code vocabulary all
 * simple_query bridges send to Dart (`docs/ERROR_MAPPING.md`), so the Dart
 * side maps one stable code set rather than Java class names (UNFY-300).
 *
 * Classification uses the exception's type hierarchy, so a subclass (e.g.
 * `NumberFormatException`, an `IllegalArgumentException`) lands with its
 * parent. Without this, Pigeon's generated `wrapError` would send the
 * exception's simple class name as the code.
 */
internal object HostErrors {
    const val PERMISSION_DENIED = "permission-denied"
    const val INVALID_QUERY = "invalid-query"
    const val UNAVAILABLE = "unavailable"
    const val TRANSIENT = "transient"
    const val NOT_SUPPORTED = "not-supported"

    private const val MESSAGE_PREFIX = "simple_query: "

    /**
     * Purpose: Convert a failure raised while serving a host call into the
     * Pigeon error Dart receives.
     *
     * @param error the failure the host call raised.
     * @return [error] itself when it is already a [QueryFlutterError] (its
     *   code is already in the vocabulary); otherwise a [QueryFlutterError]
     *   carrying the classified code, a `simple_query:` message naming the
     *   native type, and details `{nativeType, nativeStackTrace}` so the
     *   original failure is preserved.
     * @throws Nothing.
     */
    fun toFlutterError(error: Throwable): QueryFlutterError {
        if (error is QueryFlutterError) return error
        val nativeMessage = error.message
        val type = error.javaClass.simpleName
        return QueryFlutterError(
            codeFor(error),
            if (nativeMessage == null) "$MESSAGE_PREFIX$type" else "$MESSAGE_PREFIX$type: $nativeMessage",
            mapOf(
                "nativeType" to error.javaClass.name,
                "nativeStackTrace" to Log.getStackTraceString(error),
            ),
        )
    }

    /**
     * Purpose: Pick the vocabulary code for one host failure, per the table in
     * `docs/ERROR_MAPPING.md`. Specific SQLite subclasses are matched before
     * the [SQLiteException] they extend.
     *
     * @param error the failure to classify.
     * @return the vocabulary code; [TRANSIENT] for a failure no rule covers,
     *   which keeps [UNAVAILABLE] meaning only "provider unreachable".
     * @throws Nothing.
     */
    fun codeFor(error: Throwable): String = when (error) {
        is SecurityException -> PERMISSION_DENIED
        is IllegalArgumentException -> INVALID_QUERY
        is UnsupportedOperationException -> NOT_SUPPORTED
        is SQLiteConstraintException -> INVALID_QUERY
        is SQLiteDatabaseCorruptException -> UNAVAILABLE
        is SQLiteFullException,
        is SQLiteDiskIOException,
        is SQLiteDatabaseLockedException,
        is SQLiteTableLockedException -> TRANSIENT
        is SQLiteException -> INVALID_QUERY
        is OperationApplicationException -> INVALID_QUERY
        is OperationCanceledException -> TRANSIENT
        is FileNotFoundException -> UNAVAILABLE
        is RemoteException -> UNAVAILABLE
        else -> TRANSIENT
    }
}
