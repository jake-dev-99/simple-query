package io.simplezen.simple_query

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import io.flutter.plugin.common.BinaryMessenger
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.util.concurrent.AbstractExecutorService
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.function.Supplier
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/**
 * Pins StreamManager's failure and lifecycle handling (UNFY-300): the copy
 * closes the write end on every exit, each pipe end has one closer, a
 * released or disposed stream does no work, no stream error reaches a
 * disposed manager's engine, and extraction neither swallows failures nor
 * leaks its temp file.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerTest {
    /**
     * Runs copies only when told to, inline. [rejecting] makes it refuse
     * work, as a shut-down pool does.
     */
    private class ManualExecutor : AbstractExecutorService() {
        val queued = mutableListOf<Runnable>()
        var runInline = true
        var rejecting = false

        override fun execute(command: Runnable) {
            if (rejecting) throw RejectedExecutionException("shut down")
            if (runInline) command.run() else queued.add(command)
        }

        fun runQueued() {
            queued.toList().forEach { it.run() }
            queued.clear()
        }

        override fun shutdown() = Unit
        override fun shutdownNow(): MutableList<Runnable> = mutableListOf()
        override fun isShutdown() = false
        override fun isTerminated() = false
        override fun awaitTermination(timeout: Long, unit: TimeUnit) = true
    }

    /** A pipe end whose close releases the descriptor but reports failure. */
    private class FailingCloseEnd(private val inner: ParcelFileDescriptor) : ParcelFileDescriptor(inner) {
        override fun close() {
            inner.close()
            throw IOException("close failed")
        }
    }

    // Answers every open with no stream.
    class NoStreamProvider : ContentProvider() {
        override fun onCreate() = true
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? = null
        override fun query(
            uri: Uri,
            projection: Array<String>?,
            selection: String?,
            selectionArgs: Array<String>?,
            sortOrder: String?,
        ): Cursor? = null
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

    private val sentChannels = mutableListOf<String>()
    private val messenger = object : BinaryMessenger {
        override fun send(channel: String, message: ByteBuffer?) {
            sentChannels.add(channel)
        }

        override fun send(
            channel: String,
            message: ByteBuffer?,
            callback: BinaryMessenger.BinaryReply?,
        ) {
            sentChannels.add(channel)
        }

        override fun setMessageHandler(
            channel: String,
            handler: BinaryMessenger.BinaryMessageHandler?,
        ) = Unit
    }

    private val context = RuntimeEnvironment.getApplication()
    private val executor = ManualExecutor()
    private val createdPipes = mutableListOf<Array<ParcelFileDescriptor>>()
    private var wrapReadEnd: (ParcelFileDescriptor) -> ParcelFileDescriptor = { it }
    private var wrapWriteEnd: (ParcelFileDescriptor) -> ParcelFileDescriptor = { it }
    private var opens = 0

    private lateinit var manager: StreamManager

    @Before
    fun setUp() {
        Robolectric.buildContentProvider(NoStreamProvider::class.java).create(NO_STREAM_AUTHORITY)
        manager = StreamManager(
            context,
            QueryFlutterApi(messenger),
            Handler(Looper.getMainLooper()),
            executor = executor,
            createPipe = {
                val pipes = ParcelFileDescriptor.createPipe()
                arrayOf(wrapReadEnd(pipes[0]), wrapWriteEnd(pipes[1])).also { createdPipes.add(it) }
            },
        )
    }

    @Test
    fun aFailedOpenClosesTheWriteEndAndReportsAStreamError() {
        register(CONTENT) { throw SecurityException("Permission Denial") }

        manager.openStream(CONTENT)
        idle()

        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(listOf(STREAM_ERROR_CHANNEL), sentChannels)
    }

    @Test
    fun aFileNotFoundOpenClosesTheWriteEnd() {
        // FileNotFoundException is an IOException, so it takes the copy's
        // IOException branch rather than the generic one.
        register(CONTENT) { throw FileNotFoundException("No content provider") }

        manager.openStream(CONTENT)
        idle()

        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(listOf(STREAM_ERROR_CHANNEL), sentChannels)
    }

    @Test
    fun aSuccessfulCopyClosesTheWriteEndWithoutAnError() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }

        manager.openStream(CONTENT)
        idle()

        assertFalse(writeEnd().fileDescriptor.valid())
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun closeStreamClosesBothEndsAndAQueuedCopyDoesNoWork() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        executor.runInline = false
        val descriptor = manager.openStream(CONTENT)

        manager.closeStream(descriptor.streamId)

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())

        executor.runQueued()
        idle()
        assertEquals(0, opens)
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun closeStreamStopsARunningCopyWithoutAStreamError() {
        executor.runInline = false
        lateinit var streamId: String
        var reads = 0
        register(CONTENT) {
            object : InputStream() {
                override fun read(): Int = throw UnsupportedOperationException()
                override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                    reads += 1
                    // The reader gives up while the copy is mid-stream.
                    if (reads == 1) manager.closeStream(streamId)
                    // Endless content, bounded so a regression fails instead of hanging.
                    return if (reads < MAX_READS) length.coerceAtMost(4) else -1
                }
            }
        }
        streamId = manager.openStream(CONTENT).streamId

        executor.runQueued()
        idle()

        assertTrue(reads < MAX_READS, "copy kept reading after release: $reads reads")
        assertFalse(writeEnd().fileDescriptor.valid())
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun closeStreamRaisesAFailedReadEndClose() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        wrapReadEnd = { FailingCloseEnd(it) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStream(descriptor.streamId) }

        assertEquals("close failed", error.message)
        // The stream is released all the same: a second close is a no-op.
        manager.closeStream(descriptor.streamId)
    }

    @Test
    fun openStreamAfterDisposeReleasesBothEndsAndThrows() {
        manager.dispose()

        assertFailsWith<IllegalStateException> { manager.openStream(CONTENT) }

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(0, opens)
    }

    @Test
    fun aRejectedCopyWithAFailingWriteCloseStillClosesTheReadEnd() {
        executor.rejecting = true
        wrapWriteEnd = { FailingCloseEnd(it) }

        val error = assertFailsWith<RejectedExecutionException> { manager.openStream(CONTENT) }

        assertEquals("close failed", error.suppressed.single().message)
        assertFalse(readEnd().fileDescriptor.valid())
    }

    @Test
    fun aRejectedCopyReleasesBothEndsAndThrows() {
        executor.rejecting = true

        assertFailsWith<RejectedExecutionException> { manager.openStream(CONTENT) }

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    @Test
    fun disposeReleasesQueuedStreamsWhoseCopiesThenDoNoWork() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        executor.runInline = false
        manager.openStream(CONTENT)

        manager.dispose()
        executor.runQueued()
        idle()

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(0, opens)
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun aStreamErrorPostedJustBeforeDisposeIsNotSent() {
        register(CONTENT) { throw SecurityException("Permission Denial") }
        // The failed copy's report is queued on the main looper...
        manager.openStream(CONTENT)

        // ...when the plugin detaches, before that report runs.
        manager.dispose()
        idle()

        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun aStreamErrorRaisedAfterDisposeIsNotSent() {
        // The copy is in flight when the plugin detaches, then fails.
        register(CONTENT) {
            manager.dispose()
            throw SecurityException("Permission Denial")
        }

        manager.openStream(CONTENT)
        idle()

        assertTrue(sentChannels.isEmpty())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    @Test
    fun extractToFileCopiesTheContent() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(4, 5, 6)) }

        val path = checkNotNull(manager.extractToFile(CONTENT))

        val file = File(path.removePrefix("file://"))
        assertEquals(listOf<Byte>(4, 5, 6), file.readBytes().toList())
        file.delete()
    }

    @Test
    fun extractToFileReturnsNullForNoStreamAndLeavesNoTempFile() {
        val before = tempFileCount()

        assertNull(manager.extractToFile("content://$NO_STREAM_AUTHORITY/nostream"))
        assertEquals(before, tempFileCount())
    }

    @Test
    fun extractToFileRaisesACopyFailureAndLeavesNoTempFile() {
        register(CONTENT) {
            object : InputStream() {
                override fun read(): Int = throw IOException("disk read failed")
            }
        }
        val before = tempFileCount()

        val error = assertFailsWith<IOException> { manager.extractToFile(CONTENT) }

        assertEquals("disk read failed", error.message)
        assertEquals(before, tempFileCount())
    }

    private fun register(uri: String, stream: () -> InputStream) {
        shadowOf(context.contentResolver).registerInputStreamSupplier(
            Uri.parse(uri),
            Supplier {
                opens += 1
                stream()
            },
        )
    }

    private fun readEnd() = createdPipes.single()[0]

    private fun writeEnd() = createdPipes.single()[1]

    private fun idle() = shadowOf(Looper.getMainLooper()).idle()

    private fun tempFileCount() =
        context.cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0

    private companion object {
        const val CONTENT = "content://io.simplezen.simple_query.streams.test/item"
        const val NO_STREAM_AUTHORITY = "io.simplezen.simple_query.nostream.test"
        const val MAX_READS = 1000
        const val STREAM_ERROR_CHANNEL =
            "dev.flutter.pigeon.simple_query.QueryFlutterApi.onStreamError"
    }
}
