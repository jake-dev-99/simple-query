package io.simplezen.simple_query

import android.app.Application
import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.ByteArrayInputStream
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.AbstractExecutorService
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.function.Supplier
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.runBlocking
import org.robolectric.Robolectric
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.shadows.ShadowLog

/**
 * Purpose: The shared setup for the StreamManager test classes, held by
 * each class rather than inherited: a manager on a [ManualExecutor], with
 * every pipe it creates recorded in [pipes] and every provider open
 * counted in [opens].
 *
 * Build it as a test-class field, so Robolectric's application exists,
 * and call [setUp] before each test.
 *
 * @throws Nothing.
 */
internal class StreamManagerFixture {
    val context: Application = RuntimeEnvironment.getApplication()
    val executor = ManualExecutor()
    val pipes = PipeRecorder()
    var opens = 0
        private set
    lateinit var manager: StreamManager

    /**
     * Purpose: Register the no-stream provider and build the manager on
     * [executor].
     *
     * @return Nothing.
     * @throws Nothing.
     */
    fun setUp() {
        Robolectric.buildContentProvider(NoStreamProvider::class.java).create(NO_STREAM_AUTHORITY)
        manager = managerOn(executor)
    }

    /**
     * Purpose: Build a manager on [copies] whose pipes [pipes] records and
     * wraps.
     *
     * @param copies the executor that runs the background copies.
     * @param copyStopTimeoutMs how long closeStream waits for a running copy.
     * @return the manager.
     * @throws Nothing.
     */
    fun managerOn(copies: ExecutorService, copyStopTimeoutMs: Long = 5_000L) = StreamManager(
        context,
        executor = copies,
        createPipe = pipes::create,
        copyStopTimeoutMs = copyStopTimeoutMs,
    )

    /**
     * Purpose: Open a stream on a real copy thread whose copy reads all its
     * content, then waits inside its write-end close until [failClose]
     * counts down, and then fails that close.
     *
     * The write end is already released when the copy waits, so a release
     * leaves the close to the copy, and its failure arrives late. The new
     * manager replaces [manager].
     *
     * @param failClose counted down to let the write-end close fail with
     *   "close failed".
     * @param copyStopTimeoutMs how long closeStream waits for the copy.
     * @param content builds the provider content the copy reads.
     * @return the stream's id and its copy thread.
     * @throws AssertionError when the copy never reaches the write-end close.
     */
    fun openStreamWaitingInWriteEndClose(
        failClose: CountDownLatch,
        copyStopTimeoutMs: Long = 5_000L,
        content: () -> InputStream = { ByteArrayInputStream(byteArrayOf(1, 2, 3)) },
    ): Pair<String, ExecutorService> {
        register(CONTENT, content)
        val closing = CountDownLatch(1)
        pipes.wrapWriteEnd = {
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
     * Purpose: Read the stream's read end to end-of-stream on this thread,
     * as a Dart reader does on its own, without closing it.
     *
     * @param writeEndClosed counted down when the write end closes, the
     *   moment a real pipe delivers end-of-stream ([SignalingEnd]).
     * @return the number of bytes read.
     * @throws AssertionError when the write end does not close in time.
     * @throws IOException when the read fails.
     */
    fun readToEndOfStream(writeEndClosed: CountDownLatch): Int {
        assertTrue(writeEndClosed.await(5, TimeUnit.SECONDS), "the copy never closed the write end")
        val reader = FileInputStream(pipes.readEnd().fileDescriptor)
        val buffer = ByteArray(64)
        var total = 0
        while (true) {
            val read = reader.read(buffer)
            if (read == -1) return total
            total += read
        }
    }

    /**
     * Purpose: Answer every open of [uri] with a fresh [stream], counting
     * each open in [opens].
     *
     * @param uri the content URI to register.
     * @param stream builds the content for one open; it may throw to fail
     *   the open.
     * @return Nothing.
     * @throws Nothing.
     */
    fun register(uri: String, stream: () -> InputStream) {
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
    fun assertReleasedWithoutACopy() {
        pipes.assertBothEndsClosed()
        assertEquals(1, opens)
        assertTrue(executor.queued.isEmpty())
    }

    /**
     * Purpose: Return the warnings StreamManager has logged in this test.
     *
     * @return the WARN log items for the StreamManager tag.
     * @throws Nothing.
     */
    fun warnings() = ShadowLog.getLogsForTag("StreamManager").filter { it.type == Log.WARN }

    /**
     * Purpose: Assert that StreamManager logged exactly one copy failure no
     *   caller could raise, naming its stream, with [cause] attached.
     *
     * @param cause the logged failure's message.
     * @return Nothing.
     * @throws AssertionError when no warning, another warning, or more than
     *   one warning was logged.
     */
    fun assertCopyFailureLoggedOnce(cause: String) {
        val logged = warnings().single()
        assertTrue(logged.msg.startsWith("stream "), logged.msg)
        assertTrue(logged.msg.endsWith(": copy failed with no caller to raise it"), logged.msg)
        assertEquals(cause, logged.throwable?.message)
    }

    /**
     * Purpose: Count the extraction temp files in the cache directory.
     *
     * @return the number of `sq_extract_` files.
     * @throws Nothing.
     */
    fun tempFileCount() = context.cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0

    companion object {
        const val CONTENT = "content://io.simplezen.simple_query.streams.test/item"
        const val NO_STREAM_AUTHORITY = "io.simplezen.simple_query.nostream.test"
    }
}

/**
 * Purpose: Create the manager's pipes, wrap each end so a test can fake
 * its close, and record them so a test can check them.
 *
 * @throws Nothing.
 */
internal class PipeRecorder {
    private val created = mutableListOf<Array<ParcelFileDescriptor>>()
    var wrapReadEnd: (ParcelFileDescriptor) -> ParcelFileDescriptor = { it }
    var wrapWriteEnd: (ParcelFileDescriptor) -> ParcelFileDescriptor = { it }

    /**
     * Purpose: Create a pipe, wrap its ends, and record it.
     *
     * @return the wrapped read end and write end.
     * @throws IOException when the platform cannot create the pipe.
     */
    fun create(): Array<ParcelFileDescriptor> {
        val ends = ParcelFileDescriptor.createPipe()
        return arrayOf(wrapReadEnd(ends[0]), wrapWriteEnd(ends[1])).also { created.add(it) }
    }

    /**
     * Purpose: Return the read end of the test's only pipe.
     *
     * @return the read end.
     * @throws IllegalArgumentException unless exactly one pipe was created.
     */
    fun readEnd() = created.single()[0]

    /**
     * Purpose: Return the write end of the test's only pipe.
     *
     * @return the write end.
     * @throws IllegalArgumentException unless exactly one pipe was created.
     */
    fun writeEnd() = created.single()[1]

    /**
     * Purpose: Assert that both ends of the test's only pipe are closed.
     *
     * @return Nothing.
     * @throws AssertionError when either end is still open.
     */
    fun assertBothEndsClosed() {
        assertFalse(readEnd().fileDescriptor.valid())
        assertFalse(writeEnd().fileDescriptor.valid())
    }
}

/**
 * Purpose: Close [streamId] from the calling thread, as the host's IO
 * coroutine does, waiting for any running copy.
 *
 * @param streamId the stream to close.
 * @return Nothing.
 * @throws Exception whatever [StreamManager.closeStream] raises.
 */
internal fun StreamManager.closeStreamBlocking(streamId: String) = runBlocking { closeStream(streamId) }

/**
 * Purpose: Build a single real copy thread, as production runs copies,
 * whose uncaught failures go to [onUncaught].
 *
 * @param onUncaught receives a failure that escapes a copy.
 * @return the executor.
 * @throws Nothing.
 */
internal fun copyThread(onUncaught: (Throwable) -> Unit = {}): ExecutorService =
    Executors.newSingleThreadExecutor { task ->
        Thread(task).apply {
            isDaemon = true
            setUncaughtExceptionHandler { _, error -> onUncaught(error) }
        }
    }

/**
 * Purpose: An executor that runs copies only when told to, inline.
 * [rejecting] makes it refuse work, as a shut-down pool does.
 *
 * @throws RejectedExecutionException from [execute] while [rejecting].
 */
internal class ManualExecutor : AbstractExecutorService() {
    val queued = mutableListOf<Runnable>()
    var runInline = true
    var rejecting = false

    /**
     * Purpose: Run [command] now, queue it, or reject it.
     *
     * @param command the copy to run.
     * @return Nothing.
     * @throws RejectedExecutionException while [rejecting].
     */
    override fun execute(command: Runnable) {
        if (rejecting) throw RejectedExecutionException("shut down")
        if (runInline) command.run() else queued.add(command)
    }

    /**
     * Purpose: Run every queued copy on the calling thread, in order.
     *
     * @return Nothing.
     * @throws Exception whatever a copy throws.
     */
    fun runQueued() {
        queued.toList().forEach { it.run() }
        queued.clear()
    }

    /**
     * Purpose: Accept a shutdown; this executor holds no threads.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    override fun shutdown() = Unit

    /**
     * Purpose: Accept an immediate shutdown; this executor holds no threads.
     *
     * @return an empty list, as no queued work is handed back.
     * @throws Nothing.
     */
    override fun shutdownNow(): MutableList<Runnable> = mutableListOf()

    /**
     * Purpose: Report that this executor never shuts down.
     *
     * @return false.
     * @throws Nothing.
     */
    override fun isShutdown() = false

    /**
     * Purpose: Report that this executor never terminates.
     *
     * @return false.
     * @throws Nothing.
     */
    override fun isTerminated() = false

    /**
     * Purpose: Return at once; this executor holds no threads to wait for.
     *
     * @param timeout ignored.
     * @param unit ignored.
     * @return true.
     * @throws Nothing.
     */
    override fun awaitTermination(timeout: Long, unit: TimeUnit) = true
}

/**
 * Purpose: A pipe end whose close releases the descriptor but reports
 * failure.
 *
 * @param inner the end to wrap.
 * @throws IOException from every close.
 */
internal class FailingCloseEnd(private val inner: ParcelFileDescriptor) : ParcelFileDescriptor(inner) {
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
internal class SignalingEnd(
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
internal class TrackedStream(
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
internal class FailsAfterFirstChunk(private val failure: Exception) : InputStream() {
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

/**
 * Purpose: A content provider that answers every open with no stream.
 *
 * @throws Nothing.
 */
class NoStreamProvider : ContentProvider() {
    /**
     * Purpose: Start the provider.
     *
     * @return true, as there is nothing to set up.
     * @throws Nothing.
     */
    override fun onCreate() = true

    /**
     * Purpose: Answer an open with no stream.
     *
     * @param uri ignored.
     * @param mode ignored.
     * @return null.
     * @throws Nothing.
     */
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? = null

    /**
     * Purpose: Answer a query with no cursor; no test queries this provider.
     *
     * @param uri ignored.
     * @param projection ignored.
     * @param selection ignored.
     * @param selectionArgs ignored.
     * @param sortOrder ignored.
     * @return null.
     * @throws Nothing.
     */
    override fun query(
        uri: Uri,
        projection: Array<String>?,
        selection: String?,
        selectionArgs: Array<String>?,
        sortOrder: String?,
    ): Cursor? = null

    /**
     * Purpose: Report no MIME type.
     *
     * @param uri ignored.
     * @return null.
     * @throws Nothing.
     */
    override fun getType(uri: Uri): String? = null

    /**
     * Purpose: Insert nothing.
     *
     * @param uri ignored.
     * @param values ignored.
     * @return null.
     * @throws Nothing.
     */
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    /**
     * Purpose: Delete nothing.
     *
     * @param uri ignored.
     * @param selection ignored.
     * @param selectionArgs ignored.
     * @return 0.
     * @throws Nothing.
     */
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?) = 0

    /**
     * Purpose: Update nothing.
     *
     * @param uri ignored.
     * @param values ignored.
     * @param selection ignored.
     * @param selectionArgs ignored.
     * @return 0.
     * @throws Nothing.
     */
    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<String>?,
    ) = 0
}
