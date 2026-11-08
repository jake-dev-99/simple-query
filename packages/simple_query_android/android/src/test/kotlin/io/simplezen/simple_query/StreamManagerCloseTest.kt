package io.simplezen.simple_query

import io.simplezen.simple_query.StreamManagerFixture.Companion.CONTENT
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Purpose: Pin what closeStream does (UNFY-300, UNFY-365): it releases
 * both pipe ends, stops the copy, and raises the copy's failure or a
 * failed close, but not a failure the release itself caused.
 *
 * @throws AssertionError from a test whose behavior does not hold.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerCloseTest {
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
        fixture.register(CONTENT) { content }

        val descriptor = fixture.manager.openStream(CONTENT)

        assertTrue(content.reads > 0)
        assertTrue(content.closed)
        assertFalse(fixture.pipes.writeEnd().fileDescriptor.valid())
        fixture.manager.closeStreamBlocking(descriptor.streamId)
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
        fixture.register(CONTENT) {
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

        val descriptor = fixture.manager.openStream(CONTENT)

        assertEquals(1, attempts)
        assertTrue(closed)
        assertFalse(fixture.pipes.writeEnd().fileDescriptor.valid())
        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }
        assertEquals("disk read failed", error.message)
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
        fixture.register(CONTENT) { content }
        fixture.executor.runInline = false
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IllegalStateException> {
            fixture.manager.closeStreamBlocking(descriptor.streamId)
        }

        assertEquals("provider close failed", error.message)
        assertTrue(content.closed)
        fixture.pipes.assertBothEndsClosed()
    }

    /**
     * Purpose: Pin that closeStream releases both pipe ends of a stream whose
     *   copy is still queued, and that the copy then does no work.
     *
     * @return Nothing.
     * @throws AssertionError when a pipe end stays open, the content is
     *   read, or the input stays open.
     */
    @Test
    fun closeStreamClosesBothEndsAndAQueuedCopyDoesNoWork() {
        val content = TrackedStream(byteArrayOf(1))
        fixture.register(CONTENT) { content }
        fixture.executor.runInline = false
        val descriptor = fixture.manager.openStream(CONTENT)

        fixture.manager.closeStreamBlocking(descriptor.streamId)

        fixture.pipes.assertBothEndsClosed()

        fixture.executor.runQueued()
        assertEquals(1, fixture.opens)
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
        fixture.manager = fixture.managerOn(copies)
        val descriptor = fixture.manager.openStream(CONTENT)
        assertTrue(midStream.await(5, TimeUnit.SECONDS))

        fixture.manager.closeStreamBlocking(descriptor.streamId)

        assertEquals(2, reads)
        assertFalse(fixture.pipes.writeEnd().fileDescriptor.valid())
        copies.shutdown()
    }

    /**
     * Purpose: Pin that closeStream closes a copy stalled in a provider read
     *   that answers only once its stream is closed.
     *
     * @return Nothing.
     * @throws AssertionError when the copy stays blocked or the write end
     *   stays open.
     */
    @Test
    fun closeStreamUnblocksACopyStalledInAProviderRead() {
        fixture.executor.runInline = false
        val readEntered = CountDownLatch(1)
        val closed = CountDownLatch(1)
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
                 * Purpose: Never answer until the stream is closed, then fail
                 * as a closed stream does.
                 *
                 * @param buffer the buffer to fill.
                 * @param offset the first index to fill.
                 * @param length the most bytes to read.
                 * @return Nothing; it always throws.
                 * @throws IOException "Stream closed", once the stream closes.
                 */
                override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                    readEntered.countDown()
                    closed.await()
                    throw IOException("Stream closed")
                }

                /**
                 * Purpose: Release the stalled read.
                 *
                 * @return Nothing.
                 * @throws Nothing.
                 */
                override fun close() {
                    closed.countDown()
                }
            }
        }
        val descriptor = fixture.manager.openStream(CONTENT)
        val copy = Thread {
            fixture.executor.runQueued()
        }.apply {
            isDaemon = true
            start()
        }
        assertTrue(readEntered.await(5, TimeUnit.SECONDS))

        fixture.manager.closeStreamBlocking(descriptor.streamId)
        copy.join(5_000)

        assertFalse(copy.isAlive, "copy stayed blocked in the provider read")
        assertFalse(fixture.pipes.writeEnd().fileDescriptor.valid())
    }

    /**
     * Purpose: Pin that closeStream raises a failed read-end close, and that
     *   the stream is released all the same.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream does not raise the close
     *   failure, or a second close raises.
     */
    @Test
    fun closeStreamRaisesAFailedReadEndClose() {
        fixture.register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        fixture.pipes.wrapReadEnd = { FailingCloseEnd(it) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("close failed", error.message)
        // The stream is released all the same: a second close is a no-op.
        fixture.manager.closeStreamBlocking(descriptor.streamId)
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
        fixture.register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        fixture.pipes.wrapWriteEnd = { FailingCloseEnd(it) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("close failed", error.message)
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
        fixture.register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        fixture.pipes.assertBothEndsClosed()
        // The stream is released all the same: a second close is a no-op.
        fixture.manager.closeStreamBlocking(descriptor.streamId)
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
        fixture.register(CONTENT) { FailsAfterFirstChunk(IOException("Broken pipe")) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("Broken pipe", error.message)
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
        fixture.register(CONTENT) { FailsAfterFirstChunk(SecurityException("Permission Denial")) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<SecurityException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

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
        fixture.register(CONTENT) { FailsAfterFirstChunk(IOException("disk read failed")) }
        fixture.pipes.wrapReadEnd = { FailingCloseEnd(it) }
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

        assertEquals("disk read failed", error.message)
        assertEquals("close failed", error.suppressed.single().message)
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
        fixture.register(CONTENT) {
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
        val descriptor = fixture.manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { fixture.manager.closeStreamBlocking(descriptor.streamId) }

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
        fixture.register(CONTENT) {
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
        fixture.pipes.wrapWriteEnd = { SignalingEnd(it, writeEndClosed) }
        val uncaught = AtomicReference<Throwable?>(null)
        val reachedThread = CountDownLatch(1)
        val copies = copyThread { error ->
            uncaught.set(error)
            reachedThread.countDown()
        }
        fixture.manager = fixture.managerOn(copies)
        val descriptor = fixture.manager.openStream(CONTENT)

        assertEquals(0, fixture.readToEndOfStream(writeEndClosed))
        assertTrue(inputClosed.await(5, TimeUnit.SECONDS))
        fixture.manager.closeStreamBlocking(descriptor.streamId)

        assertTrue(reachedThread.await(5, TimeUnit.SECONDS))
        assertEquals("provider stack", (uncaught.get() as StackOverflowError).message)
        copies.shutdown()
    }
}
