package io.simplezen.simple_query

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.AbstractExecutorService
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import java.util.function.Supplier
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowLog

/**
 * Pins StreamManager's failure and lifecycle handling (UNFY-300): the copy
 * closes the write end on every exit, each pipe end has one closer, a
 * released or disposed stream does no work, a copy failure is raised by
 * closeStream (UNFY-365), and extraction neither swallows failures nor
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
        /**
         * Purpose: Release the descriptor, then report a failed close.
         *
         * @return Nothing; it always throws.
         * @throws IOException on every call.
         */
        override fun close() {
            inner.close()
            throw IOException("close failed")
        }
    }

    /**
     * Purpose: A pipe end that counts down [closed] once its descriptor is
     * released, then runs [afterClose].
     *
     * Robolectric's createPipe backs both ends with one temp file, so a
     * reader sees end-of-file whenever it catches up with the writer. A real
     * pipe's reader sees end-of-stream only when the write end closes, so
     * threaded tests wait on [closed] to read at that moment.
     *
     * @param inner the end to wrap.
     * @param closed counted down after [inner] closes.
     * @param afterClose runs after [closed] is counted down.
     * @throws Exception whatever [afterClose] throws.
     */
    private class SignalingEnd(
        private val inner: ParcelFileDescriptor,
        private val closed: CountDownLatch,
        private val afterClose: () -> Unit = {},
    ) : ParcelFileDescriptor(inner) {
        /**
         * Purpose: Release the descriptor, signal it, then run [afterClose].
         *
         * @return Nothing.
         * @throws Exception whatever [afterClose] throws.
         */
        override fun close() {
            inner.close()
            closed.countDown()
            afterClose()
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
     * Purpose: Provider content whose first read returns 4 bytes and whose
     * next read fails the copy partway through.
     *
     * @param failure thrown by every read after the first.
     * @throws Exception [failure], from every read after the first.
     */
    private class FailsAfterFirstChunk(private val failure: Exception) : InputStream() {
        private var reads = 0

        /**
         * Purpose: Reject single-byte reads; the copy reads in chunks.
         *
         * @return Nothing; it always throws.
         * @throws UnsupportedOperationException on every call.
         */
        override fun read(): Int = throw UnsupportedOperationException()

        /**
         * Purpose: Return 4 bytes on the first read, then fail.
         *
         * @param b the buffer to fill.
         * @param off the first index to fill.
         * @param len the most bytes to read.
         * @return 4 on the first read.
         * @throws Exception [failure], on every later read.
         */
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            reads += 1
            if (reads > 1) throw failure
            return 4
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
        manager = managerOn(executor)
    }

    /**
     * Purpose: Build a manager on [copies] whose pipes this test records and
     * wraps.
     *
     * @param copies the executor that runs the background copies.
     * @param copyStopTimeoutMs how long closeStream waits for a running copy.
     * @return the manager.
     * @throws Nothing.
     */
    private fun managerOn(copies: ExecutorService, copyStopTimeoutMs: Long = 5_000L) = StreamManager(
        context,
        executor = copies,
        createPipe = {
            val pipes = ParcelFileDescriptor.createPipe()
            arrayOf(wrapReadEnd(pipes[0]), wrapWriteEnd(pipes[1])).also { createdPipes.add(it) }
        },
        copyStopTimeoutMs = copyStopTimeoutMs,
    )

    /**
     * Purpose: Open a stream on a real copy thread whose copy reads all its
     * content, then waits inside its write-end close until [failClose]
     * counts down, and then fails that close.
     *
     * The write end is already released when the copy waits, so a release
     * leaves the close to the copy, and its failure arrives late.
     *
     * @param failClose counted down to let the write-end close fail with
     *   "close failed".
     * @param copyStopTimeoutMs how long closeStream waits for the copy.
     * @return the stream's id and its copy thread.
     * @throws AssertionError when the copy never reaches the write-end close.
     */
    private fun openStreamWaitingInWriteEndClose(
        failClose: CountDownLatch,
        copyStopTimeoutMs: Long = 5_000L,
    ): Pair<String, ExecutorService> {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        val closing = CountDownLatch(1)
        wrapWriteEnd = {
            SignalingEnd(it, closing) {
                failClose.await(5, TimeUnit.SECONDS)
                throw IOException("close failed")
            }
        }
        val copies = copyThread()
        manager = managerOn(copies, copyStopTimeoutMs)
        val descriptor = manager.openStream(CONTENT)
        assertTrue(closing.await(5, TimeUnit.SECONDS))
        return descriptor.streamId to copies
    }

    /**
     * Purpose: Build a single real copy thread, as production runs copies,
     * whose uncaught failures go to [onUncaught].
     *
     * @param onUncaught receives a failure that escapes a copy.
     * @return the executor.
     * @throws Nothing.
     */
    private fun copyThread(onUncaught: (Throwable) -> Unit = {}): ExecutorService =
        Executors.newSingleThreadExecutor { task ->
            Thread(task).apply {
                isDaemon = true
                setUncaughtExceptionHandler { _, error -> onUncaught(error) }
            }
        }

    /**
     * Purpose: Close [streamId] from this test thread, as the host's IO
     * coroutine does, waiting for any running copy.
     *
     * @param streamId the stream to close.
     * @return Nothing.
     * @throws Exception whatever [StreamManager.closeStream] raises.
     */
    private fun StreamManager.closeStreamBlocking(streamId: String) = runBlocking { closeStream(streamId) }

    /**
     * Purpose: Read the stream's read end to end-of-stream on this thread,
     * as a Dart reader does on its own, without closing it.
     *
     * @param writeEndClosed counted down when the write end closes, the
     *   moment a real pipe delivers end-of-stream ([SignalingEnd]).
     * @return the number of bytes read.
     * @throws AssertionError when the write end does not close in time.
     * @throws IOException when the read fails.
     */
    private fun readToEndOfStream(writeEndClosed: CountDownLatch): Int {
        assertTrue(writeEndClosed.await(5, TimeUnit.SECONDS), "the copy never closed the write end")
        val reader = FileInputStream(readEnd().fileDescriptor)
        val buffer = ByteArray(64)
        var total = 0
        while (true) {
            val read = reader.read(buffer)
            if (read == -1) return total
            total += read
        }
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

        val error = assertFailsWith<IllegalStateException> { manager.closeStreamBlocking(descriptor.streamId) }

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
     *   opened, and closes the write end (UNFY-364), and that closeStream
     *   then raises nothing (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when the copy skips the read, leaves the input
     *   or the write end open, or closeStream raises.
     */
    @Test
    fun aSuccessfulCopyClosesBothTheInputAndTheWriteEndWithoutAnError() {
        val content = TrackedStream(byteArrayOf(1, 2, 3))
        register(CONTENT) { content }

        val descriptor = manager.openStream(CONTENT)

        assertTrue(content.reads > 0)
        assertTrue(content.closed)
        assertFalse(writeEnd().fileDescriptor.valid())
        manager.closeStreamBlocking(descriptor.streamId)
    }

    /**
     * Purpose: Pin that a copy whose read fails still closes the input and the
     *   write end (UNFY-364), and closeStream raises the failure (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when the read is skipped, the input or the
     *   write end stays open, or closeStream does not raise the failure.
     */
    @Test
    fun aFailedCopyStillClosesTheInputAndClosesTheStreamWithItsFailure() {
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

        val descriptor = manager.openStream(CONTENT)

        assertEquals(1, attempts)
        assertTrue(closed)
        assertFalse(writeEnd().fileDescriptor.valid())
        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }
        assertEquals("disk read failed", error.message)
    }

    @Test
    fun closeStreamClosesBothEndsAndAQueuedCopyDoesNoWork() {
        val content = TrackedStream(byteArrayOf(1))
        register(CONTENT) { content }
        executor.runInline = false
        val descriptor = manager.openStream(CONTENT)

        manager.closeStreamBlocking(descriptor.streamId)

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())

        executor.runQueued()
        assertEquals(1, opens)
        assertEquals(0, content.reads)
        assertTrue(content.closed)
    }

    /**
     * Purpose: Pin that closeStream during a running copy stops it, waits
     *   for it, and raises nothing: the read the release cuts short fails
     *   with "Stream closed", and that failure is the release itself.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream raises the release's own
     *   failure, which happens when recordFailure's caller drops its
     *   released check, or the copy keeps running.
     */
    @Test
    fun closeStreamStopsARunningCopyWithoutRaisingItsFailure() {
        val midStream = CountDownLatch(1)
        val inputClosed = CountDownLatch(1)
        var reads = 0
        register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Reject single-byte reads; the copy reads in chunks.
                 *
                 * @return Nothing; it always throws.
                 * @throws UnsupportedOperationException on every call.
                 */
                override fun read(): Int = throw UnsupportedOperationException()

                /**
                 * Purpose: Return 4 bytes, then wait mid-stream until the
                 * release closes this input, and fail as a closed stream does.
                 *
                 * @param buffer the buffer to fill.
                 * @param offset the first index to fill.
                 * @param length the most bytes to read.
                 * @return 4 on the first read.
                 * @throws IOException "Stream closed", on every later read.
                 */
                override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                    reads += 1
                    if (reads == 1) return 4
                    // Mid-stream: the reader gives up while this read waits.
                    midStream.countDown()
                    assertTrue(inputClosed.await(5, TimeUnit.SECONDS))
                    throw IOException("Stream closed")
                }

                /**
                 * Purpose: Signal that the release closed this input.
                 *
                 * @return Nothing.
                 * @throws Nothing.
                 */
                override fun close() {
                    inputClosed.countDown()
                }
            }
        }
        val copies = copyThread()
        manager = managerOn(copies)
        val descriptor = manager.openStream(CONTENT)
        assertTrue(midStream.await(5, TimeUnit.SECONDS))

        manager.closeStreamBlocking(descriptor.streamId)

        assertEquals(2, reads)
        assertFalse(writeEnd().fileDescriptor.valid())
        copies.shutdown()
    }

    @Test
    fun closeStreamRaisesAFailedReadEndClose() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        wrapReadEnd = { FailingCloseEnd(it) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("close failed", error.message)
        // The stream is released all the same: a second close is a no-op.
        manager.closeStreamBlocking(descriptor.streamId)
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

                override fun close() {
                    closed.countDown()
                }
            }
        }
        val descriptor = manager.openStream(CONTENT)
        val copy = Thread {
            executor.runQueued()
        }.apply {
            isDaemon = true
            start()
        }
        assertTrue(readEntered.await(5, TimeUnit.SECONDS))

        manager.closeStreamBlocking(descriptor.streamId)
        copy.join(5_000)

        assertFalse(copy.isAlive, "copy stayed blocked in the provider read")
        assertFalse(writeEnd().fileDescriptor.valid())
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

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(1, opens)
        assertEquals(0, content.reads)
        assertTrue(content.closed)
    }

    /**
     * Purpose: Pin that a provider read failing with "Broken pipe" is a copy
     *   failure, not a reader that closed early, so closeStream raises it
     *   (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream does not raise the failure.
     */
    @Test
    fun closeStreamRaisesAProviderBrokenPipeFromTheCopy() {
        register(CONTENT) { FailsAfterFirstChunk(IOException("Broken pipe")) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("Broken pipe", error.message)
    }

    /**
     * Purpose: Pin that a copy failing partway through makes closeStream
     *   raise that failure after releasing both ends (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream does not raise the failure, a
     *   pipe end stays open, or a second close raises.
     */
    @Test
    fun closeStreamRaisesTheRecordedCopyFailure() {
        register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        // The stream is released all the same: a second close is a no-op.
        manager.closeStreamBlocking(descriptor.streamId)
    }

    /**
     * Purpose: Pin that a provider SecurityException during the copy is
     *   raised by closeStream as thrown, so it classifies as permissionDenied
     *   (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream does not raise the
     *   SecurityException.
     */
    @Test
    fun closeStreamRaisesAProviderSecurityExceptionFromTheCopy() {
        register(CONTENT) { FailsAfterFirstChunk(SecurityException("Permission Denial")) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<SecurityException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("Permission Denial", error.message)
    }

    /**
     * Purpose: Pin that a failed pipe close during closeStream is attached
     *   to the recorded copy failure as suppressed (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when the copy failure is not raised or the
     *   close failure is not attached to it.
     */
    @Test
    fun closeStreamAttachesACloseFailureToTheCopyFailure() {
        register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        wrapReadEnd = { FailingCloseEnd(it) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        assertEquals("close failed", error.suppressed.single().message)
    }

    /**
     * Purpose: Pin that a failed write-end close at the end of a successful
     *   copy is raised by closeStream (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream does not raise the close
     *   failure.
     */
    @Test
    fun closeStreamRaisesAFailedWriteEndCloseFromTheCopy() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        wrapWriteEnd = { FailingCloseEnd(it) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("close failed", error.message)
    }

    /**
     * Purpose: Pin that dispose() releases a stream whose copy failed without
     *   throwing, and logs that failure with its stream id (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when dispose() throws, a pipe end stays open,
     *   or the copy failure is not logged.
     */
    @Test
    fun disposeReleasesAFailedCopyWithoutThrowing() {
        register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        manager.openStream(CONTENT)

        manager.dispose()

        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertCopyFailureLoggedOnce("disk read failed")
    }

    /**
     * Purpose: Pin the real thread order: the copy fails on its own thread,
     *   the reader reads to end-of-stream on another, then closeStream
     *   raises the failure (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream returns normally.
     */
    @Test
    fun closeStreamRaisesACopyFailureAfterTheReaderSawEndOfStreamOnAnotherThread() {
        register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        val writeEndClosed = CountDownLatch(1)
        wrapWriteEnd = { SignalingEnd(it, writeEndClosed) }
        val copies = copyThread()
        manager = managerOn(copies)
        val descriptor = manager.openStream(CONTENT)

        assertEquals(4, readToEndOfStream(writeEndClosed))
        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        copies.shutdown()
    }

    /**
     * Purpose: Pin that a write-end close which delivers end-of-stream and
     *   then fails is raised by a closeStream the reader calls at once,
     *   because closeStream waits for the copy to record it (UNFY-365).
     *
     * The failing close reports only after the release has closed the read
     * end, so closeStream is always inside that window.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream returns normally.
     */
    @Test
    fun closeStreamRaisesAWriteEndCloseFailureThatRacesTheRelease() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        val writeEndClosed = CountDownLatch(1)
        val readEndClosed = CountDownLatch(1)
        wrapReadEnd = { SignalingEnd(it, readEndClosed) }
        wrapWriteEnd = {
            SignalingEnd(it, writeEndClosed) {
                readEndClosed.await(5, TimeUnit.SECONDS)
                throw IOException("close failed")
            }
        }
        val copies = copyThread()
        manager = managerOn(copies)
        val descriptor = manager.openStream(CONTENT)

        assertEquals(3, readToEndOfStream(writeEndClosed))
        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("close failed", error.message)
        copies.shutdown()
    }

    /**
     * Purpose: Pin that a copy failure caught before a release is raised by
     *   that release, even when the release lands while the copy is still
     *   closing its input (UNFY-365).
     *
     * The input's close runs only after the copy caught the failure, and
     * waits until the release has closed the read end.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream returns normally.
     */
    @Test
    fun closeStreamRaisesACopyFailureCaughtBeforeARacingRelease() {
        val closingInput = CountDownLatch(1)
        val readEndClosed = CountDownLatch(1)
        register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Reject single-byte reads; the copy reads in chunks.
                 *
                 * @return Nothing; it always throws.
                 * @throws UnsupportedOperationException on every call.
                 */
                override fun read(): Int = throw UnsupportedOperationException()

                /**
                 * Purpose: Fail the copy's first read.
                 *
                 * @param buffer the buffer to fill.
                 * @param offset the first index to fill.
                 * @param length the most bytes to read.
                 * @return Nothing; it always throws.
                 * @throws IOException on every call.
                 */
                override fun read(buffer: ByteArray, offset: Int, length: Int): Int =
                    throw IOException("disk read failed")

                /**
                 * Purpose: Hold the copy's input close until the release has
                 * closed the read end.
                 *
                 * @return Nothing.
                 * @throws Nothing.
                 */
                override fun close() {
                    closingInput.countDown()
                    readEndClosed.await(5, TimeUnit.SECONDS)
                }
            }
        }
        wrapReadEnd = { SignalingEnd(it, readEndClosed) }
        val copies = copyThread()
        manager = managerOn(copies)
        val descriptor = manager.openStream(CONTENT)
        assertTrue(closingInput.await(5, TimeUnit.SECONDS))

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        copies.shutdown()
    }

    /**
     * Purpose: Pin that a copy failure recorded after dispose() is logged
     *   once, by the copy, since dispose() does not wait for it (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when the failure is logged before it happens,
     *   not logged, or logged twice.
     */
    @Test
    fun aCopyFailingAfterDisposeLogsItsFailureOnce() {
        val failClose = CountDownLatch(1)
        val (_, copies) = openStreamWaitingInWriteEndClose(failClose)

        manager.dispose()
        assertTrue(warnings().isEmpty())
        failClose.countDown()

        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        assertCopyFailureLoggedOnce("close failed")
    }

    /**
     * Purpose: Pin that a closeStream cancelled while it waits for the copy
     *   leaves the copy's later failure to the log, once (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when the failure is lost or logged twice.
     */
    @Test
    fun aCancelledCloseStreamLeavesALaterCopyFailureToTheLog() {
        val failClose = CountDownLatch(1)
        val (streamId, copies) = openStreamWaitingInWriteEndClose(failClose)

        runBlocking {
            // Unconfined runs closeStream here until it suspends in its wait.
            launch(Dispatchers.Unconfined) { manager.closeStream(streamId) }.cancelAndJoin()
        }
        assertTrue(warnings().isEmpty())
        failClose.countDown()

        copies.shutdown()
        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        assertCopyFailureLoggedOnce("close failed")
    }

    /**
     * Purpose: Pin that closeStream raises a copy that does not stop in time
     *   as incomplete content, and leaves the copy's later failure to the
     *   log, once (UNFY-365).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream returns normally, or the later
     *   failure is lost or logged twice.
     */
    @Test
    fun closeStreamRaisesACopyThatDoesNotStopAndLogsItsLaterFailure() {
        val failClose = CountDownLatch(1)
        val (streamId, copies) = openStreamWaitingInWriteEndClose(failClose, copyStopTimeoutMs = 100)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(streamId) }

        assertTrue(error.message.orEmpty().contains("did not stop within 100ms"), error.message)
        assertTrue(warnings().isEmpty())
        failClose.countDown()
        copies.shutdown()
        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        assertCopyFailureLoggedOnce("close failed")
    }

    /**
     * Purpose: Pin that a provider rethrowing one cached exception from both
     *   its read and its close is raised as that exception, with nothing
     *   suppressed onto it (Kotlin's addSuppressed skips the exception
     *   itself).
     *
     * @return Nothing.
     * @throws AssertionError when closeStream raises anything else.
     */
    @Test
    fun closeStreamRaisesAFailureTheProviderRethrowsFromItsClose() {
        val cached = IOException("provider failed")
        register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Fail the copy's read with [cached].
                 *
                 * @return Nothing; it always throws.
                 * @throws IOException [cached], on every call.
                 */
                override fun read(): Int = throw cached

                /**
                 * Purpose: Fail the close with the same [cached] exception.
                 *
                 * @return Nothing; it always throws.
                 * @throws IOException [cached], on every call.
                 */
                override fun close() = throw cached
            }
        }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStreamBlocking(descriptor.streamId) }

        assertTrue(error === cached)
        assertTrue(error.suppressed.isEmpty())
    }

    /**
     * Purpose: Pin that a JVM Error in the copy propagates on the copy's own
     *   thread, after the input and write end close, instead of being
     *   recorded and raised later by closeStream as a retryable failure.
     *
     * @return Nothing.
     * @throws AssertionError when the Error does not reach the copy thread's
     *   uncaught-exception handler, an end stays open, or closeStream
     *   raises it.
     */
    @Test
    fun aJvmErrorInTheCopyPropagatesOnTheCopyThread() {
        val inputClosed = CountDownLatch(1)
        register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Fail the copy's read with a JVM Error.
                 *
                 * @return Nothing; it always throws.
                 * @throws StackOverflowError on every call.
                 */
                override fun read(): Int = throw StackOverflowError("provider stack")

                /**
                 * Purpose: Signal that the copy closed this input.
                 *
                 * @return Nothing.
                 * @throws Nothing.
                 */
                override fun close() {
                    inputClosed.countDown()
                }
            }
        }
        val writeEndClosed = CountDownLatch(1)
        wrapWriteEnd = { SignalingEnd(it, writeEndClosed) }
        val uncaught = AtomicReference<Throwable?>(null)
        val reachedThread = CountDownLatch(1)
        val copies = copyThread { error ->
            uncaught.set(error)
            reachedThread.countDown()
        }
        manager = managerOn(copies)
        val descriptor = manager.openStream(CONTENT)

        assertEquals(0, readToEndOfStream(writeEndClosed))
        assertTrue(inputClosed.await(5, TimeUnit.SECONDS))
        manager.closeStreamBlocking(descriptor.streamId)

        assertTrue(reachedThread.await(5, TimeUnit.SECONDS))
        assertEquals("provider stack", (uncaught.get() as StackOverflowError).message)
        copies.shutdown()
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
     * no pipe end open and queued no copy.
     *
     * @return Nothing.
     * @throws AssertionError when any of those does not hold.
     */
    private fun assertReleasedWithoutACopy() {
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
        assertEquals(1, opens)
        assertTrue(executor.queued.isEmpty())
    }

    /**
     * Purpose: Return the warnings StreamManager has logged in this test.
     *
     * @return the WARN log items for the StreamManager tag.
     * @throws Nothing.
     */
    private fun warnings() = ShadowLog.getLogsForTag("StreamManager").filter { it.type == Log.WARN }

    /**
     * Purpose: Assert that StreamManager logged exactly one copy failure no
     *   caller could raise, naming its stream, with [cause] attached.
     *
     * @param cause the logged failure's message.
     * @return Nothing.
     * @throws AssertionError when no warning, another warning, or more than
     *   one warning was logged.
     */
    private fun assertCopyFailureLoggedOnce(cause: String) {
        val logged = warnings().single()
        assertTrue(logged.msg.startsWith("stream "), logged.msg)
        assertTrue(logged.msg.endsWith(": copy failed with no caller to raise it"), logged.msg)
        assertEquals(cause, logged.throwable?.message)
    }

    private fun readEnd() = createdPipes.single()[0]

    private fun writeEnd() = createdPipes.single()[1]

    private fun tempFileCount() =
        context.cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0

    private companion object {
        const val CONTENT = "content://io.simplezen.simple_query.streams.test/item"
        const val NO_STREAM_AUTHORITY = "io.simplezen.simple_query.nostream.test"
    }
}
