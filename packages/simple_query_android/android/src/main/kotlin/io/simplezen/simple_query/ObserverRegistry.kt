package io.simplezen.simple_query

import android.content.Context
import android.database.ContentObserver
import android.net.Uri
import android.os.Handler
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

private const val NOTIFY_INSERT = 4
private const val NOTIFY_UPDATE = 8
private const val NOTIFY_DELETE = 16

// Android permits combined operations. Claim a specific type only when it is
// unambiguous; modifier flags must not obscure that operation.
internal fun contentChangeTypeFromFlags(flags: Int): ContentChangeType =
    when (flags and (NOTIFY_INSERT or NOTIFY_UPDATE or NOTIFY_DELETE)) {
        NOTIFY_INSERT -> ContentChangeType.INSERT
        NOTIFY_UPDATE -> ContentChangeType.UPDATE
        NOTIFY_DELETE -> ContentChangeType.DELETE
        else -> ContentChangeType.UNKNOWN
    }

/**
 * Registry for ContentObserver instances.
 *
 * Manages observer lifecycle and dispatches change events to Flutter.
 * Automatically cleans up observers on Activity detach to prevent leaks.
 */
class ObserverRegistry(
    private val context: Context,
    private val flutterApi: QueryFlutterApi,
    private val mainHandler: Handler,
) {
    private val contentResolver get() = context.contentResolver
    private val observers = ConcurrentHashMap<String, RegisteredObserver>()

    /** Orders a registration's final step against [dispose]; never held across IPC. */
    private val lifecycleLock = Any()
    @Volatile private var disposed = false

    private data class RegisteredObserver(
        val observerId: String,
        val contentUri: String,
        val observer: ContentObserver,
    )

    /**
     * Register a new ContentObserver for the given URI.
     *
     * @param contentUri The content URI to observe
     * @param notifyForDescendants If true, also observe changes to descendant URIs
     * @return The observer ID for later unregistration
     * @throws IllegalStateException when the registry is disposed (plugin
     *   detached), so no observer outlives [dispose] (UNFY-300).
     */
    fun register(contentUri: String, notifyForDescendants: Boolean): String {
        val observerId = UUID.randomUUID().toString()
        val uri = Uri.parse(contentUri)

        val observer = object : ContentObserver(mainHandler) {
            override fun onChange(selfChange: Boolean) {
                dispatchChange(
                    observerId, contentUri, null,
                    ContentChangeType.UNKNOWN,
                    flags = null,
                    selfChange = selfChange,
                )
            }

            override fun onChange(selfChange: Boolean, uri: Uri?) {
                dispatchChange(
                    observerId,
                    contentUri,
                    uri?.toString(),
                    ContentChangeType.UNKNOWN,
                    flags = null,
                    selfChange = selfChange,
                )
            }

            override fun onChange(selfChange: Boolean, uri: Uri?, flags: Int) {
                dispatchChange(
                    observerId, contentUri, uri?.toString(),
                    contentChangeTypeFromFlags(flags),
                    flags = flags,
                    selfChange = selfChange,
                )
            }

            override fun onChange(selfChange: Boolean, uris: Collection<Uri>, flags: Int) {
                for (changedUri in uris) {
                    dispatchChange(
                        observerId, contentUri, changedUri.toString(),
                        contentChangeTypeFromFlags(flags),
                        flags = flags,
                        selfChange = selfChange,
                    )
                }
            }
        }

        check(!disposed) { "simple_query: observer registry is disposed" }
        contentResolver.registerContentObserver(uri, notifyForDescendants, observer)

        // The binder registration above runs without the lock, so a
        // concurrent dispose() never waits on it. Whichever comes second
        // under the lock undoes the registration.
        val accepted = synchronized(lifecycleLock) {
            if (!disposed) {
                observers[observerId] = RegisteredObserver(
                    observerId = observerId,
                    contentUri = contentUri,
                    observer = observer,
                )
            }
            !disposed
        }
        if (!accepted) {
            contentResolver.unregisterContentObserver(observer)
            throw IllegalStateException("simple_query: observer registry is disposed")
        }

        return observerId
    }

    /**
     * Unregister a specific observer.
     */
    fun unregister(observerId: String) {
        val registered = observers.remove(observerId) ?: return
        contentResolver.unregisterContentObserver(registered.observer)
    }

    /**
     * Unregister all observers.
     * Called on Activity detach to prevent leaks; observers may register again.
     */
    fun unregisterAll() {
        val ids = observers.keys.toList()
        for (id in ids) {
            unregister(id)
        }
    }

    /**
     * Purpose: Unregister every observer on plugin detach and refuse new ones.
     *
     * A registration in flight either recorded its observer first, and is
     * unregistered here, or sees the registry disposed and undoes itself
     * (UNFY-300). The unregistrations run outside [lifecycleLock].
     *
     * @return Nothing.
     * @throws Nothing.
     */
    fun dispose() {
        val registered = synchronized(lifecycleLock) {
            disposed = true
            observers.values.toList().also { observers.clear() }
        }
        for (entry in registered) {
            contentResolver.unregisterContentObserver(entry.observer)
        }
    }

    private fun dispatchChange(
        observerId: String,
        registeredUri: String,
        changedUri: String?,
        changeType: ContentChangeType,
        flags: Int?,
        selfChange: Boolean,
    ) {
        // Already on main thread via mainHandler. A change queued before its
        // observer was unregistered (or the registry disposed) has no Dart
        // listener left to receive it.
        if (!observers.containsKey(observerId)) return
        flutterApi.onContentChange(
            ContentChangeEvent(
                observerId = observerId,
                uri = changedUri ?: registeredUri,
                changeType = changeType,
                // Pigeon expects Long? for flags.
                flags = flags?.toLong(),
                selfChange = selfChange,
            )
        ) { /* ignore callback result */ }
    }

}
