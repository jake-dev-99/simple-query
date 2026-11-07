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
import java.util.concurrent.CountDownLatch
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

    /**
     * Purpose: Provider content that records whether it was read and
     * closed, and fails a read after close as a real provider stream does.
     *
     * @param bytes the content.
     * @param closeFailure thrown by close, after the close is recorded;
     *   null for a close that succeeds.
     * @throws IOException from a read after close.
     */
    private class TrackedStream(
        bytes: ByteArray,
        private val closeFailure: Exception? = null,
    ) : ByteArrayInputStream(bytes) {
        var reads = 0
        var closed = false

        /**
         * Purpose: Read like [ByteArrayInputStream], counting each read and
         * failing once the stream is closed.
         *
         * @param b the buffer to fill.
         * @param off the first index to fill.
         * @param len the most bytes to read.
         * @return the bytes read, or -1 at end of stream.
         * @throws IOException when the stream is closed.
         */
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (closed) throw IOException("Stream closed")
            reads += 1
            return super.read(b, off, len)
        }

        /**
         * Purpose: Record the close, then fail it when [closeFailure] is set.
         *
         * @return Nothing.
         * @throws Exception [closeFailure], when set.
         */
        override fun close() {
            closed = true
            closeFailure?.let { throw it }
        }
    }

    /**
     * Purpose: Provider content whose first read runs [beforeFailing], then
     * fails the copy, counting each read attempt.
     *
     * @param beforeFailing work the read does before it fails.
     * @throws IOException from every read.
     */
    private class FailingRead(private val beforeFailing: () -> Unit) : InputStream() {
        var attempts = 0

        /**
         * Purpose: Count the attempt, run [beforeFailing], then fail the read.
         *
         * @return Nothing; it always throws.
         * @throws IOException on every call.
         */
        override fun read(): Int {
            attempts += 1
            beforeFailing()
            throw IOException("disk read failed")
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

    /**
     * Purpose: Pin that a provider SecurityException at open fails openStream,
     *   releases both pipe ends and queues no copy (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the failure is swallowed, a pipe end
     *   stays open, or a copy is queued.
     */
    @Test
    fun openStreamRaisesAProviderOpenFailureAndReleasesBothEnds() {
        register(CONTENT) { throw SecurityException("Permission Denial") }
        executor.runInline = false

        assertFailsWith<SecurityException> { manager.openStream(CONTENT) }

        assertReleasedWithoutACopy()
    }

    /**
     * Purpose: Pin that a provider FileNotFoundException at open fails
     *   openStream, releases both pipe ends and queues no copy (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the failure is swallowed, a pipe end
     *   stays open, or a copy is queued.
     */
    @Test
    fun openStreamRaisesAFileNotFoundOpenFailureAndReleasesBothEnds() {
        register(CONTENT) { throw FileNotFoundException("No content provider") }
        executor.runInline = false

        assertFailsWith<FileNotFoundException> { manager.openStream(CONTENT) }

        assertReleasedWithoutACopy()
    }

    /**
     * Purpose: Pin that a provider returning no stream fails openStream as
     *   unavailable with `reason: noStream` and releases both pipe ends
     *   (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the code or details differ, a pipe end
     *   stays open, or a copy is queued.
     */
    @Test
    fun openStreamRaisesUnavailableWhenTheProviderReturnsNoStream() {
        val uri = "content://$NO_STREAM_AUTHORITY/nostream"
        executor.runInline = false

        val error = assertFailsWith<QueryFlutterError> { manager.openStream(uri) }

        assertEquals(HostErrors.UNAVAILABLE, error.code)
        assertEquals(mapOf("reason" to "noStream", "contentUri" to uri), error.details)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertTrue(executor.queued.isEmpty())
    }

    /**
     * Purpose: Pin that an unchecked failure closing the input still lets
     *   closeStream close both pipe ends, then raises it.
     *
     * @return Nothing.
     * @throws AssertionError when the failure is swallowed or a pipe end
     *   stays open.
     */
    @Test
    fun closeStreamClosesBothEndsWhenTheInputCloseThrowsAnUncheckedFailure() {
        val content = TrackedStream(byteArrayOf(1), closeFailure = IllegalStateException("provider close failed"))
        register(CONTENT) { content }
        executor.runInline = false
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IllegalStateException> { manager.closeStream(descriptor.streamId) }

        assertEquals("provider close failed", error.message)
        assertTrue(content.closed)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    /**
     * Purpose: Pin that dispose() logs an unchecked failure closing an input
     *   and still releases the stream's pipe ends.
     *
     * @return Nothing.
     * @throws AssertionError when dispose() throws or a pipe end stays
     *   open.
     */
    @Test
    fun disposeContinuesPastAnUncheckedInputCloseFailure() {
        val content = TrackedStream(byteArrayOf(1), closeFailure = IllegalStateException("provider close failed"))
        register(CONTENT) { content }
        executor.runInline = false
        manager.openStream(CONTENT)

        manager.dispose()

        assertTrue(content.closed)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    /**
     * Purpose: Pin that a dispose() during the provider open closes the opened
     *   input and fails openStream (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the input or a pipe end stays open, or
     *   openStream returns.
     */
    @Test
    fun openStreamClosesTheOpenedInputWhenDisposeRacesTheOpen() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) {
            // The plugin detaches while the provider is opening the content.
            manager.dispose()
            content
        }

        assertFailsWith<IllegalStateException> { manager.openStream(CONTENT) }

        assertTrue(content.closed)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    /**
     * Purpose: Pin that a failed input close during the dispose race is kept
     *   as suppressed on the IllegalStateException (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the close failure replaces or drops
     *   the dispose failure, or a pipe end stays open.
     */
    @Test
    fun aDisposeRacingTheOpenKeepsAFailedInputCloseAsSuppressed() {
        val content = TrackedStream(byteArrayOf(1), closeFailure = IOException("provider close failed"))
        register(CONTENT) {
            manager.dispose()
            content
        }

        val error = assertFailsWith<IllegalStateException> { manager.openStream(CONTENT) }

        assertEquals("provider close failed", error.suppressed.single().message)
        assertTrue(content.closed)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }

    /**
     * Purpose: Pin that a finished copy reads and closes the input openStream
     *   opened, and closes the write end (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the copy skips the read, leaves the input
     *   or the write end open, or reports an error.
     */
    @Test
    fun aSuccessfulCopyClosesBothTheInputAndTheWriteEndWithoutAnError() {
        val content = TrackedStream(byteArrayOf(1, 2, 3))
        register(CONTENT) { content }

        manager.openStream(CONTENT)
        idle()

        assertTrue(content.reads > 0)
        assertTrue(content.closed)
        assertFalse(writeEnd().fileDescriptor.valid())
        assertTrue(sentChannels.isEmpty())
    }

    /**
     * Purpose: Pin that a copy whose read fails still closes the input and the
     *   write end, and reports one stream error (UNFY-364).
     *
     * @return Nothing.
     * @throws AssertionError when the read is skipped, the input or the
     *   write end stays open, or no stream error is sent.
     */
    @Test
    fun aFailedCopyStillClosesTheInputAndReportsAStreamError() {
        var closed = false
        var attempts = 0
        register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Count the attempt, then fail the copy's read.
                 *
                 * @return Nothing; it always throws.
                 * @throws IOException on every call.
                 */
                override fun read(): Int {
                    attempts += 1
                    throw IOException("disk read failed")
                }

                /**
                 * Purpose: Record that the copy closed the input.
                 *
                 * @return Nothing.
                 * @throws Nothing.
                 */
                override fun close() {
                    closed = true
                }
            }
        }

        manager.openStream(CONTENT)
        idle()

        assertEquals(1, attempts)
        assertTrue(closed)
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(listOf(STREAM_ERROR_CHANNEL), sentChannels)
    }

    @Test
    fun closeStreamClosesBothEndsAndAQueuedCopyDoesNoWork() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) { content }
        executor.runInline = false
        val descriptor = manager.openStream(CONTENT)

        manager.closeStream(descriptor.streamId)

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())

        executor.runQueued()
        idle()
        assertEquals(1, opens)
        assertEquals(0, content.reads)
        assertTrue(content.closed)
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
    fun closeStreamUnblocksACopyStalledInAProviderRead() {
        executor.runInline = false
        val readEntered = CountDownLatch(1)
        val closed = CountDownLatch(1)
        register(CONTENT) {
            object : InputStream() {
                override fun read(): Int = throw UnsupportedOperationException()
                override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                    readEntered.countDown()
                    // A provider that never answers until its stream is closed.
                    closed.await()
                    throw IOException("Stream closed")
                }

                override fun close() = closed.countDown()
            }
        }
        val descriptor = manager.openStream(CONTENT)
        val copy = Thread { executor.runQueued() }.apply {
            isDaemon = true
            start()
        }
        assertTrue(readEntered.await(5, TimeUnit.SECONDS))

        manager.closeStream(descriptor.streamId)
        copy.join(5_000)

        assertFalse(copy.isAlive, "copy stayed blocked in the provider read")
        assertFalse(writeEnd().fileDescriptor.valid())
        idle()
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun aRejectedCopyWithAFailingWriteCloseStillClosesTheReadEnd() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) { content }
        executor.rejecting = true
        wrapWriteEnd = { FailingCloseEnd(it) }

        val error = assertFailsWith<RejectedExecutionException> { manager.openStream(CONTENT) }

        assertEquals("close failed", error.suppressed.single().message)
        assertFalse(readEnd().fileDescriptor.valid())
        assertTrue(content.closed)
    }

    @Test
    fun aRejectedCopyReleasesBothEndsAndThrows() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) { content }
        executor.rejecting = true

        assertFailsWith<RejectedExecutionException> { manager.openStream(CONTENT) }

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertTrue(content.closed)
    }

    @Test
    fun disposeReleasesQueuedStreamsWhoseCopiesThenDoNoWork() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) { content }
        executor.runInline = false
        manager.openStream(CONTENT)

        manager.dispose()
        executor.runQueued()
        idle()

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(1, opens)
        assertEquals(0, content.reads)
        assertTrue(content.closed)
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun aStreamErrorPostedJustBeforeDisposeIsNotSent() {
        val content = FailingRead {}
        register(CONTENT) { content }
        // The failed copy's report is queued on the main looper...
        manager.openStream(CONTENT)

        // ...when the plugin detaches, before that report runs.
        manager.dispose()
        idle()

        assertEquals(1, content.attempts)
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun aStreamErrorRaisedAfterDisposeIsNotSent() {
        // The copy is in flight when the plugin detaches, then fails.
        val content = FailingRead { manager.dispose() }
        register(CONTENT) { content }

        manager.openStream(CONTENT)
        idle()

        assertEquals(1, content.attempts)
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

    @Test
    fun extractToFileLeavesNoTempFileWhenTheProviderStreamFailsToClose() {
        register(CONTENT) {
            object : ByteArrayInputStream(byteArrayOf(7, 8)) {
                override fun close() = throw IOException("provider close failed")
            }
        }
        val before = tempFileCount()

        val error = assertFailsWith<IOException> { manager.extractToFile(CONTENT) }

        assertEquals("provider close failed", error.message)
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

    /**
     * Purpose: Assert that a failed openStream opened the content once, left
     * no pipe end open, queued no copy and sent no stream error.
     *
     * @return Nothing.
     * @throws AssertionError when any of those does not hold.
     */
    private fun assertReleasedWithoutACopy() {
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(1, opens)
        assertTrue(executor.queued.isEmpty())
        idle()
        assertTrue(sentChannels.isEmpty())
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
