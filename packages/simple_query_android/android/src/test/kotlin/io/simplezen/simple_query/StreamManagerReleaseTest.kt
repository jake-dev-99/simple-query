package io.simplezen.simple_query

import io.simplezen.simple_query.StreamManagerFixture.Companion.CONTENT
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.asCoroutineDispatcher
import org.junit.After
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Purpose: Pin how closeStream's release, which runs on its own worker,
 * meets its deadline and the copy it stops (UNFY-365).
 *
 * @throws AssertionError from a test whose behavior does not hold.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerReleaseTest {
    private val fixture = StreamManagerFixture()
    private val closer = Executors.newSingleThreadExecutor()

    /**
     * Purpose: Build a fresh manager for each test.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    @Before
    fun setUp() = fixture.setUp()

    /**
     * Purpose: Stop the thread each test closes its stream on.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    @After
    fun tearDown() = closer.shutdown()

    /**
     * Purpose: Pin that closeStream meets its deadline when the release
     *   blocks in a provider close while the copy is stalled, and logs the
     *   close failure that arrives later, once.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream waits for the blocked close,
     *   raises something other than the timeout, or the later close failure
     *   is not logged once.
     */
    @Test(timeout = 10_000)
    fun closeStreamMeetsItsDeadlineWhenTheReleaseBlocksInAProviderClose() {
        val reading = CountDownLatch(1)
        val unstall = CountDownLatch(1)
        fixture.register(CONTENT) { StallingStream(reading, unstall) }
        val copies = copyThread()
        val releases = Executors.newSingleThreadExecutor()
        fixture.manager = fixture.managerOn(copies, closeTimeoutMs = 100, releases = releases.asCoroutineDispatcher())
        val streamId = fixture.manager.openStream(CONTENT).streamId
        assertTrue(reading.await(5, TimeUnit.SECONDS))

        val outcome = closeOnAnotherThread(streamId)
        val error = try {
            assertIs<IOException>(outcome.get(2, TimeUnit.SECONDS))
        } finally {
            unstall.countDown()
        }

        assertTrue(error.message.orEmpty().contains("did not finish within 100ms"), error.message)
        awaitCondition("the release logs its close failure") { fixture.errors().isNotEmpty() }
        fixture.assertCloseFailureLoggedOnce("provider close failed")
        assertTrue(fixture.warnings().isEmpty())
        shutDown(copies, releases)
    }

    /**
     * Purpose: Pin that a non-positive timeout still lets closeStream see a
     *   copy and a release that have already finished, instead of reporting
     *   a successful copy as incomplete.
     *
     * The manual executor runs the copy inside openStream, and the default
     * release runs inline, so nothing is left to wait for.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream raises, or a pipe end stays
     *   open.
     */
    @Test
    fun aZeroTimeoutStillSeesAStreamThatAlreadyFinished() {
        fixture.register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
        fixture.manager = fixture.managerOn(fixture.executor, closeTimeoutMs = 0)
        val streamId = fixture.manager.openStream(CONTENT).streamId

        fixture.manager.closeStreamBlocking(streamId)

        fixture.pipes.assertBothEndsClosed()
    }

    /**
     * Purpose: Pin that a read failure the copy catches after claim() marked
     *   the stream released, but before the release took a resource, is
     *   raised rather than taken for one the release caused.
     *
     * The release worker is held busy, so the release is queued, and so
     * the stream marked released, before the read fails, and it closes
     * nothing until the copy has finished.
     *
     * @return Nothing.
     * @throws AssertionError when closeStream returns normally.
     */
    @Test(timeout = 10_000)
    fun closeStreamRaisesAReadFailureCaughtBeforeTheReleaseTookAResource() {
        val reading = CountDownLatch(1)
        val failRead = CountDownLatch(1)
        fixture.register(CONTENT) { FailsWhenTold(reading, failRead) }
        val writeEndClosed = CountDownLatch(1)
        fixture.pipes.wrapWriteEnd = { SignalingEnd(it, writeEndClosed) }
        val releaseThread = Executors.newSingleThreadExecutor()
        val holdRelease = CountDownLatch(1)
        releaseThread.execute { holdRelease.await(5, TimeUnit.SECONDS) }
        val releaseQueued = CountDownLatch(1)
        val releases = Executor { task ->
            releaseQueued.countDown()
            releaseThread.execute(task)
        }
        val copies = copyThread()
        fixture.manager = fixture.managerOn(copies, releases = releases.asCoroutineDispatcher())
        val streamId = fixture.manager.openStream(CONTENT).streamId
        assertTrue(reading.await(5, TimeUnit.SECONDS))

        val outcome = closeOnAnotherThread(streamId)
        assertTrue(releaseQueued.await(5, TimeUnit.SECONDS))
        failRead.countDown()
        assertTrue(writeEndClosed.await(5, TimeUnit.SECONDS), "the copy never finished")
        holdRelease.countDown()

        assertEquals("disk read failed", outcome.get(5, TimeUnit.SECONDS)?.message)
        shutDown(copies, releaseThread)
    }

    /**
     * Purpose: Close [streamId] on [closer], as the host's IO coroutine
     * does, so the test thread can act while closeStream waits.
     *
     * @param streamId the stream to close.
     * @return the failure closeStream raises, or null when it returns.
     * @throws Nothing.
     */
    private fun closeOnAnotherThread(streamId: String): Future<Throwable?> = closer.submit<Throwable?> {
        runCatching { fixture.manager.closeStreamBlocking(streamId) }.exceptionOrNull()
    }

    /**
     * Purpose: Stop [executors] and wait for their threads to finish.
     *
     * @param executors the executors to stop.
     * @return Nothing.
     * @throws AssertionError when a thread does not finish in time.
     */
    private fun shutDown(vararg executors: ExecutorService) {
        executors.forEach { it.shutdown() }
        executors.forEach { assertTrue(it.awaitTermination(5, TimeUnit.SECONDS)) }
    }
}

/**
 * Purpose: Provider content whose read stalls until [unstall] counts down,
 * and whose close blocks until then too, then fails, as a provider that
 * hangs in cleanup does.
 *
 * @param reading counted down when the copy starts reading.
 * @param unstall counted down to end the read and fail the close.
 * @throws IOException "provider close failed", from the close.
 */
private class StallingStream(
    private val reading: CountDownLatch,
    private val unstall: CountDownLatch,
) : InputStream() {
    /**
     * Purpose: Reject single-byte reads; the copy reads in chunks.
     *
     * @return Nothing; it always throws.
     * @throws UnsupportedOperationException on every call.
     */
    override fun read(): Int = throw UnsupportedOperationException()

    /**
     * Purpose: Stall until [unstall] counts down, then report end of
     * stream.
     *
     * @param b ignored.
     * @param off ignored.
     * @param len ignored.
     * @return -1.
     * @throws Nothing.
     */
    override fun read(b: ByteArray, off: Int, len: Int): Int {
        reading.countDown()
        unstall.await(5, TimeUnit.SECONDS)
        return -1
    }

    /**
     * Purpose: Block until [unstall] counts down, then fail.
     *
     * @return Nothing; it always throws.
     * @throws IOException "provider close failed".
     */
    override fun close() {
        unstall.await(5, TimeUnit.SECONDS)
        throw IOException("provider close failed")
    }
}

/**
 * Purpose: Provider content whose read waits until [fail] counts down,
 * then fails the copy on its own.
 *
 * @param reading counted down when the copy starts reading.
 * @param fail counted down to fail the read.
 * @throws IOException "disk read failed", from the read.
 */
private class FailsWhenTold(
    private val reading: CountDownLatch,
    private val fail: CountDownLatch,
) : InputStream() {
    /**
     * Purpose: Reject single-byte reads; the copy reads in chunks.
     *
     * @return Nothing; it always throws.
     * @throws UnsupportedOperationException on every call.
     */
    override fun read(): Int = throw UnsupportedOperationException()

    /**
     * Purpose: Wait until [fail] counts down, then fail.
     *
     * @param b ignored.
     * @param off ignored.
     * @param len ignored.
     * @return Nothing; it always throws.
     * @throws IOException "disk read failed".
     */
    override fun read(b: ByteArray, off: Int, len: Int): Int {
        reading.countDown()
        fail.await(5, TimeUnit.SECONDS)
        throw IOException("disk read failed")
    }
}
