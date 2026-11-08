package io.simplezen.simple_query

import io.simplezen.simple_query.StreamManagerFixture.Companion.CONTENT
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.CoroutineContext
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Purpose: Pin that a copy failure on a real copy thread is raised or
 * logged exactly once (UNFY-365): closeStream raises one that races the
 * release, and the copy logs one that arrives after dispose(), after a
 * cancelled closeStream, or after closeStream stopped waiting.
 *
 * @throws AssertionError from a test whose behavior does not hold.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerLateFailureTest {
    private val fixture = StreamManagerFixture()

    /**
     * Purpose: Build a fresh manager for each test.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    @Before
    fun setUp() = fixture.setUp()

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
        fixture.register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        val writeEndClosed = CountDownLatch(1)
        fixture.pipes.wrapWriteEnd = { SignalingEnd(it, writeEndClosed) }
        val copies = copyThread()
        fixture.manager = fixture.managerOn(copies)
        val descriptor = fixture.manager.openStream(CONTENT)

        assertEquals(4, fixture.readToEndOfStream(writeEndClosed))
        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

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
        fixture.register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        val writeEndClosed = CountDownLatch(1)
        val readEndClosed = CountDownLatch(1)
        fixture.pipes.wrapReadEnd = { SignalingEnd(it, readEndClosed) }
        fixture.pipes.wrapWriteEnd = {
            SignalingEnd(it, writeEndClosed) {
                readEndClosed.await(5, TimeUnit.SECONDS)
                throw IOException("close failed")
            }
        }
        val copies = copyThread()
        fixture.manager = fixture.managerOn(copies)
        val descriptor = fixture.manager.openStream(CONTENT)

        assertEquals(3, fixture.readToEndOfStream(writeEndClosed))
        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

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
        fixture.register(CONTENT) {
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
        fixture.pipes.wrapReadEnd = { SignalingEnd(it, readEndClosed) }
        val copies = copyThread()
        fixture.manager = fixture.managerOn(copies)
        val descriptor = fixture.manager.openStream(CONTENT)
        assertTrue(closingInput.await(5, TimeUnit.SECONDS))

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

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
        val (_, copies) = fixture.openStreamWaitingInWriteEndClose(failClose)

        fixture.manager.dispose()
        assertTrue(fixture.warnings().isEmpty())
        failClose.countDown()

        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        fixture.assertCopyFailureLoggedOnce("close failed")
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
        val (streamId, copies) = fixture.openStreamWaitingInWriteEndClose(failClose)

        runBlocking {
            // Unconfined runs closeStream here until it suspends in its wait.
            launch(Dispatchers.Unconfined) { fixture.manager.closeStream(streamId) }.cancelAndJoin()
        }
        assertTrue(fixture.warnings().isEmpty())
        failClose.countDown()

        copies.shutdown()
        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        fixture.assertCopyFailureLoggedOnce("close failed")
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
        val (streamId, copies) = fixture.openStreamWaitingInWriteEndClose(failClose, copyStopTimeoutMs = 100)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(streamId) }

        assertTrue(error.message.orEmpty().contains("did not stop within 100ms"), error.message)
        assertTrue(fixture.warnings().isEmpty())
        failClose.countDown()
        copies.shutdown()
        assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        fixture.assertCopyFailureLoggedOnce("close failed")
    }

    /**
     * Purpose: Pin that a copy finishing just as closeStream's wait times
     *   out has its failures raised, not logged (UNFY-365).
     *
     * The copy records "disk read failed", then waits in its write-end
     * close. closeStream resumes from its timed-out wait only after the copy
     * has finished, which is the narrowest gap between the timeout and the
     * hand-off to the log.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream raises only the timeout, or a
     *   failure reaches the log.
     */
    @Test
    fun closeStreamRaisesTheFailuresOfACopyThatFinishesAsItsWaitTimesOut() {
        val failClose = CountDownLatch(1)
        val (streamId, copies) = fixture.openStreamWaitingInWriteEndClose(
            failClose,
            copyStopTimeoutMs = 100,
            content = { FailsAfterFirstChunk(IOException("disk read failed")) },
        )
        val resumeAfterTheCopy = RunFirstOnResume {
            failClose.countDown()
            copies.shutdown()
            assertTrue(copies.awaitTermination(5, TimeUnit.SECONDS))
        }

        val outcome = runBlocking {
            // Undispatched, so the only dispatch is the timed-out wait's resume.
            async(resumeAfterTheCopy, CoroutineStart.UNDISPATCHED) {
                runCatching { fixture.manager.closeStream(streamId) }.exceptionOrNull()
            }.await()
        }
        resumeAfterTheCopy.close()

        val error = assertIs<IOException>(outcome)
        assertEquals("disk read failed", error.message)
        val suppressed = error.suppressed.map { it.message.orEmpty() }
        assertTrue(suppressed.contains("close failed"), "$suppressed")
        assertTrue(suppressed.any { it.contains("did not stop within 100ms") }, "$suppressed")
        assertTrue(fixture.warnings().isEmpty())
    }
}

/**
 * Purpose: Dispatch coroutines on one thread, running [beforeFirstResume]
 * just before the first one, so a test controls what happens while a
 * coroutine is about to resume.
 *
 * @param beforeFirstResume runs once, on the dispatch thread.
 * @throws Nothing.
 */
private class RunFirstOnResume(private val beforeFirstResume: () -> Unit) : CoroutineDispatcher(), AutoCloseable {
    private val thread = Executors.newSingleThreadExecutor()
    private val pending = AtomicBoolean(true)

    /**
     * Purpose: Run [block] on the dispatch thread, after [beforeFirstResume]
     * the first time.
     *
     * @param context the coroutine's context.
     * @param block the coroutine's next step.
     * @return Nothing.
     * @throws Nothing.
     */
    override fun dispatch(context: CoroutineContext, block: Runnable) {
        thread.execute {
            if (pending.getAndSet(false)) beforeFirstResume()
            block.run()
        }
    }

    /**
     * Purpose: Stop the dispatch thread.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    override fun close() = thread.shutdown()
}
