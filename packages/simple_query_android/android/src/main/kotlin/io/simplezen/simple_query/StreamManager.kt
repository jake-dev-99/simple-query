package io.simplezen.simple_query

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Purpose: Hold the workers a [StreamManager] runs on.
 *
 * Copies and releases run apart. A release closes the ends a stalled copy
 * is blocked on, so it must never queue behind the copies it stops. A
 * release that blocks in a provider close must not hold closeStream past
 * its deadline either.
 *
 * @param copies runs the background copies, and alone bounds how many run
 *   at once: the default pool runs 8 and queues the rest.
 * @param releaseDispatcher runs each closeStream's release.
 * @throws Nothing.
 */
class StreamWorkers(
    val copies: ExecutorService = Executors.newFixedThreadPool(MAX_CONCURRENT_STREAMS),
    releaseDispatcher: CoroutineDispatcher = Dispatchers.IO,
) {
    /**
     * Runs the releases. It is never cancelled, so a release closeStream
     * stopped waiting for still finishes and logs its failures.
     */
    val releases = CoroutineScope(SupervisorJob() + releaseDispatcher)

    private companion object {
        const val MAX_CONCURRENT_STREAMS = 8
    }
}

/**
 * Purpose: Stream binary content to Dart through pipes, which avoids the
 * MethodChannel size limit.
 *
 * Each stream copies the provider content into a pipe from
 * ParcelFileDescriptor.createPipe() on a background thread.
 *
 * No pipe end is closed twice from two threads (UNFY-300): the write end
 * closes through [Pipe.closeWriteEndOnce], and only whoever removes a
 * stream from [activeStreams] (closeStream, dispose, or a failed
 * openStream) closes its read end.
 *
 * A copy that fails is not reported on its own: the failure is recorded
 * and raised by closeStream (UNFY-365), so a reader that saw end-of-stream
 * learns from closeStream whether the content is complete. closeStream
 * waits for the stream's release and running copy to finish first, without
 * blocking a thread, so a failure the copy records while the reader is
 * already closing is still raised. A failure no caller can raise any more
 * (after dispose(), or after closeStream stopped waiting) is logged once
 * instead.
 *
 * @param context the context whose content resolver opens the content.
 * @param workers runs the copies and the releases; injectable so tests can
 *   control them.
 * @param createPipe makes each stream's pipe pair; injectable so tests can
 *   observe the ends.
 * @param closeTimeoutMs how long closeStream waits for the release and a
 *   running copy; injectable so tests can time it out.
 * @throws Nothing.
 */
class StreamManager(
    private val context: Context,
    private val workers: StreamWorkers = StreamWorkers(),
    private val createPipe: () -> Array<ParcelFileDescriptor> = { ParcelFileDescriptor.createPipe() },
    private val closeTimeoutMs: Long = CLOSE_TIMEOUT_MS,
) {
    companion object {
        private const val TAG = "StreamManager"

        /**
         * How long closeStream waits for a stream's release and running copy
         * to finish.
         */
        private const val CLOSE_TIMEOUT_MS = 5_000L
    }

    private val contentResolver get() = context.contentResolver
    private val activeStreams = ConcurrentHashMap<String, StreamInfo>()
    private val disposed = AtomicBoolean(false)

    /**
     * Purpose: Hold one stream's pipe pair, and close its write end exactly
     * once, whichever thread (the copy, or a release) gets there first.
     *
     * @param readEnd the end the reader reads; only a release closes it.
     * @param writeEnd the end the copy writes.
     * @throws Nothing.
     */
    private class Pipe(val readEnd: ParcelFileDescriptor, val writeEnd: ParcelFileDescriptor) {
        private val writeClosed = AtomicBoolean(false)

        /** True once a thread has taken the write end to close it. */
        val writeEndTaken: Boolean get() = writeClosed.get()

        /**
         * Purpose: Close [writeEnd] unless another thread already did.
         *
         * @return Nothing.
         * @throws IOException when the platform reports a failed close.
         */
        fun closeWriteEndOnce() {
            if (writeClosed.compareAndSet(false, true)) writeEnd.close()
        }
    }

    /**
     * Purpose: Track one stream's background copy: whether it runs, when it
     * finishes, and the failures it leaves for closeStream or the log.
     *
     * @throws Nothing.
     */
    private class CopyState {
        /**
         * Claimed by the copy when it starts, or by a release before it
         * starts; a copy claimed by a release never runs.
         */
        val claimed = AtomicBoolean(false)

        /**
         * Completed once the copy has finished, including its write-end
         * close, or once closeStream claimed it before it started.
         */
        val finished = CompletableDeferred<Unit>()

        private val failures = mutableListOf<Exception>()

        /** Set once no caller will raise a failure; each later one goes straight to it. */
        private var orphanSink: ((Exception) -> Unit)? = null

        /**
         * Purpose: Record one copy failure, in the order it happened, or
         * hand it to the orphan sink once no caller will raise it.
         *
         * @param error the failure, kept as thrown so [HostErrors] classifies
         *   it by type.
         * @return Nothing.
         * @throws Nothing.
         */
        fun record(error: Exception) {
            val sink = synchronized(this) { orphanSink.also { if (it == null) failures.add(error) } }
            sink?.invoke(error)
        }

        /**
         * Purpose: Hand every failure recorded so far, and every later one,
         * to [sink], because no caller will raise them any more.
         *
         * The sink is set and the recorded failures taken under one lock, so
         * each failure goes to [sink] exactly once, whether or not the copy
         * has finished.
         *
         * @param sink receives each failure.
         * @return Nothing.
         * @throws Nothing.
         */
        fun orphanFailuresTo(sink: (Exception) -> Unit) {
            val recorded = synchronized(this) {
                orphanSink = sink
                takeFailure()
            }
            recorded?.let(sink)
        }

        /**
         * Purpose: Take the recorded failures out, so each is raised or
         * logged exactly once.
         *
         * @return the first failure with the later ones suppressed onto it,
         *   or null when none is left.
         * @throws Nothing.
         */
        @Synchronized
        fun takeFailure(): Exception? = firstWithRestSuppressed(failures.toList()).also { failures.clear() }
    }

    /**
     * Purpose: Hold one open stream: its content, pipe, provider input and
     * copy.
     *
     * @param contentUri the content being streamed.
     * @param pipe the stream's pipe pair.
     * @throws Nothing.
     */
    private class StreamInfo(val contentUri: String, val pipe: Pipe) {
        /** Set once the stream is released; its copy then stops, or never starts. */
        val released = AtomicBoolean(false)

        /**
         * The provider stream openStream opened, until the copy or a release
         * closes it; a release closes it to unblock a stalled read or a copy
         * that never runs.
         */
        val activeInput = AtomicReference<InputStream?>(null)

        /** The stream's background copy. */
        val copy = CopyState()

        /**
         * Purpose: Tell whether a release has started closing a resource the
         * copy uses.
         *
         * Each release step takes its resource before closing it, so this
         * turns true before the copy can see a failure the release caused.
         *
         * @return true once the provider input or the write end was taken.
         * @throws Nothing.
         */
        fun releaseTookAResource() = activeInput.get() == null || pipe.writeEndTaken
    }

    /**
     * Purpose: Hold one closeStream call's released stream and its release,
     * which runs on [StreamWorkers.releases].
     *
     * @param streamId the released stream.
     * @param streamInfo the released stream's state.
     * @param release the release, resolving to its close failures.
     * @throws Nothing.
     */
    private class Closing(
        val streamId: String,
        val streamInfo: StreamInfo,
        val release: Deferred<List<Exception>>,
    ) {
        /**
         * Purpose: Tell whether the release and the copy have both finished.
         *
         * @return true when nothing is left to wait for.
         * @throws Nothing.
         */
        fun finished() = release.isCompleted && streamInfo.copy.finished.isCompleted

        /**
         * Purpose: Wait for the release and the copy to finish.
         *
         * @return Nothing.
         * @throws Error a JVM error from the release.
         */
        suspend fun awaitFinish() {
            release.await()
            streamInfo.copy.finished.await()
        }
    }

    /**
     * Purpose: Open a streaming pipe for reading binary content.
     *
     * The provider content is opened before the pipe is returned, so a
     * failure to open it fails this call (UNFY-364); only the copy runs in
     * the background. Read the content from the returned pipePath
     * (`File(pipePath).inputStream()`) and call closeStream(streamId) when
     * done.
     *
     * @param contentUri the content to stream.
     * @return the stream's id and pipe path.
     * @throws Exception whatever opening the content raises, so the host
     *   boundary classifies it ([HostErrors]).
     * @throws QueryFlutterError with [HostErrors.UNAVAILABLE] and details
     *   `{reason: noStream, contentUri}` when the provider returns no stream.
     * @throws IllegalStateException when this manager is disposed before or
     *   during the open.
     * @throws RejectedExecutionException when the executor refuses the
     *   copy. In every case the stream's pipes and any opened input are
     *   released before throwing.
     */
    fun openStream(contentUri: String): StreamDescriptor {
        val streamId = UUID.randomUUID().toString()

        // Create a pipe pair
        val pipes = createPipe()
        val readPipe = pipes[0]
        val writePipe = pipes[1]

        // Use the file descriptor path for reading
        val pipePath = "/proc/self/fd/${readPipe.fd}"

        val streamInfo = StreamInfo(contentUri, Pipe(readEnd = readPipe, writeEnd = writePipe))
        // Registered before the disposed check, so a concurrent dispose()
        // either sees and releases this stream or this check sees it disposed.
        activeStreams[streamId] = streamInfo

        // A disposed manager never opens the provider.
        if (disposed.get()) throw abandon(streamId, streamInfo, disposedError())

        val input = try {
            contentResolver.openInputStream(Uri.parse(contentUri))
        } catch (e: Throwable) {
            throw abandon(streamId, streamInfo, e)
        } ?: throw abandon(streamId, streamInfo, noStreamError(contentUri))
        streamInfo.activeInput.set(input)
        // A dispose() during the open released the stream before there was
        // an input to close. Both closers take the input with getAndSet, so
        // it closes exactly once.
        if (streamInfo.released.get()) {
            val error = disposedError()
            try {
                streamInfo.activeInput.getAndSet(null)?.close()
            } catch (e: Exception) {
                error.addSuppressed(e)
            }
            throw abandon(streamId, streamInfo, error)
        }

        try {
            workers.copies.execute { runCopy(streamInfo) }
        } catch (e: RejectedExecutionException) {
            throw abandon(streamId, streamInfo, e)
        }

        return StreamDescriptor(
            streamId = streamId,
            pipePath = pipePath,
        )
    }

    /**
     * Purpose: Extract content to a temporary file.
     *
     * Caller is responsible for deleting the file when done.
     *
     * @param contentUri the content to copy.
     * @return the file:// URI of the temp file, or null when the provider
     *   hands back no stream.
     * @throws Exception whatever opening or copying raises, so the host
     *   boundary classifies it ([HostErrors], UNFY-300). The temp file is
     *   deleted whenever no URI is returned.
     */
    fun extractToFile(contentUri: String): String? {
        val input = contentResolver.openInputStream(Uri.parse(contentUri)) ?: return null
        // Non-null until the file is delivered, so any failure (including the
        // provider stream's own close) deletes it.
        var undelivered: File? = null
        try {
            val file = input.use {
                File.createTempFile("sq_extract_", null, context.cacheDir).also { created ->
                    undelivered = created
                    FileOutputStream(created).use { output -> it.copyTo(output) }
                }
            }
            undelivered = null
            return "file://${file.absolutePath}"
        } finally {
            undelivered?.delete()
        }
    }

    /**
     * Purpose: Release a stream: stop its copy, close both pipe ends, wait
     * for the release and a running copy to finish, and raise a failure the
     * copy recorded (UNFY-365).
     *
     * Closing the write end unblocks a copy stuck writing to a reader that
     * stopped reading; closing the provider stream unblocks one stuck in a
     * stalled provider read. A copy this release stops raises nothing.
     * Waiting for the copy means a failure it records after the reader saw
     * end-of-stream, such as a failed write-end close, is still raised. The
     * release runs on [StreamWorkers.releases], so one deadline covers a
     * provider close that blocks too. The wait suspends; it does not block
     * the calling thread.
     *
     * @param streamId the stream to release; an unknown id is a no-op.
     * @return Nothing.
     * @throws Exception the copy's recorded failure, after both ends were
     *   attempted, with any later failure attached as suppressed.
     * @throws IOException when the release and the copy did not finish
     *   within [closeTimeoutMs] and the copy recorded no failure; a failure
     *   either reports later is logged.
     * @throws IOException when the platform reports a failed close and no
     *   earlier failure applies, after both ends were attempted (UNFY-300).
     * @throws RuntimeException when the provider stream's close fails with
     *   one and no earlier failure applies, after both ends were attempted.
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting; the close failures and the copy's failures are logged.
     */
    suspend fun closeStream(streamId: String) {
        val streamInfo = claim(streamId) ?: return
        // A copy claimed here never starts, so it has nothing left to finish.
        if (streamInfo.copy.claimed.compareAndSet(false, true)) streamInfo.copy.finished.complete(Unit)
        val closing = Closing(streamId, streamInfo, workers.releases.async { releaseEnds(streamInfo) })
        raiseFirst(awaitFailures(closing))
    }

    /**
     * Purpose: Release every stream on plugin detach, and refuse new ones.
     *
     * No caller remains to act on a failed close or a failed copy, so each
     * is logged with its stream id and the rest still close. This runs on
     * the main thread, so it does not wait for running copies: a copy that
     * finishes later logs its own failure.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    fun dispose() {
        disposed.set(true)
        for (id in activeStreams.keys.toList()) {
            val streamInfo = claim(id) ?: continue
            logCloseFailures(id, releaseEnds(streamInfo))
            logCopyFailuresFromNowOn(id, streamInfo.copy)
        }
        workers.copies.shutdown()
    }

    /**
     * Purpose: Mark a stream released and take it out of [activeStreams], so
     * exactly one caller releases its ends.
     *
     * The stream is marked before it is claimed, so a copy starting in
     * between skips its work. Marking it closes nothing, so a copy failure
     * caught before the release takes a resource is still recorded.
     *
     * @param streamId the stream to claim.
     * @return the stream, or null when the id is unknown or another caller
     *   claimed it first.
     * @throws Nothing.
     */
    private fun claim(streamId: String): StreamInfo? {
        val streamInfo = activeStreams[streamId] ?: return null
        streamInfo.released.set(true)
        return streamInfo.takeIf { activeStreams.remove(streamId, it) }
    }

    /**
     * Purpose: Wait for a closing stream's release and copy to finish, then
     * collect the failures closeStream raises.
     *
     * @param closing the closing stream.
     * @return the copy's failure, then an [IOException] when the wait timed
     *   out, then the close failures; failures that arrive later are logged.
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting; every failure is then left to the log.
     */
    private suspend fun awaitFailures(closing: Closing): List<Exception> {
        val finished = try {
            awaitFinish(closing)
        } catch (e: CancellationException) {
            logCopyFailuresFromNowOn(closing.streamId, closing.streamInfo.copy)
            logCloseFailuresWhenReleased(closing)
            throw e
        }
        // The copy's own failure explains the content best, then a stream
        // that would not finish, then a failed close.
        if (finished) return listOfNotNull(closing.streamInfo.copy.takeFailure()) + closing.release.await()
        return unfinishedFailures(closing)
    }

    /**
     * Purpose: Wait for a closing stream's release and copy to finish,
     * within [closeTimeoutMs].
     *
     * A non-positive timeout skips the wait, so whatever already finished is
     * checked first.
     *
     * @param closing the closing stream.
     * @return true when both finished; false when the wait timed out.
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting.
     */
    private suspend fun awaitFinish(closing: Closing): Boolean =
        closing.finished() || withTimeoutOrNull(closeTimeoutMs) { closing.awaitFinish() } != null

    /**
     * Purpose: Collect what closeStream can still raise for a stream whose
     * release or copy did not finish in time, and hand the rest to the log.
     *
     * Each failure is taken before the rest is handed off, so a release or
     * copy that finishes in between cannot log a failure this call raises.
     *
     * @param closing the closing stream.
     * @return the copy's failure so far, the timeout, then the close
     *   failures when the release finished.
     * @throws Nothing.
     */
    private suspend fun unfinishedFailures(closing: Closing): List<Exception> {
        val copyFailure = closing.streamInfo.copy.takeFailure()
        logCopyFailuresFromNowOn(closing.streamId, closing.streamInfo.copy)
        val closeFailures = if (closing.release.isCompleted) closing.release.await() else null
        if (closeFailures == null) logCloseFailuresWhenReleased(closing)
        val timeout = IOException(
            "simple_query: closing the stream of ${closing.streamInfo.contentUri} did not finish " +
                "within ${closeTimeoutMs}ms; treat the content as incomplete",
        )
        return listOfNotNull(copyFailure, timeout) + closeFailures.orEmpty()
    }

    /**
     * Purpose: Throw the first of [failures], with the rest attached to it
     * as suppressed.
     *
     * @param failures the failures, most informative first.
     * @return Nothing when [failures] is empty.
     * @throws Exception the first of [failures].
     */
    private fun raiseFirst(failures: List<Exception>) {
        firstWithRestSuppressed(failures)?.let { throw it }
    }

    /**
     * Purpose: Build the failure openStream raises once this manager is
     * disposed.
     *
     * @return a new [IllegalStateException].
     * @throws Nothing.
     */
    private fun disposedError() = IllegalStateException("simple_query: stream manager is disposed")

    /**
     * Purpose: Build the failure openStream raises when the provider returns
     * no stream for [contentUri], so it reaches Dart as `unavailable`, the
     * same code a null cursor gets (`docs/ERROR_MAPPING.md`).
     *
     * @param contentUri the content the provider returned no stream for.
     * @return a [QueryFlutterError] with [HostErrors.UNAVAILABLE] and details
     *   `{reason: noStream, contentUri}`.
     * @throws Nothing.
     */
    private fun noStreamError(contentUri: String) = QueryFlutterError(
        HostErrors.UNAVAILABLE,
        "simple_query: content provider returned no stream for $contentUri; " +
            "check that the content still exists, then retry",
        mapOf("reason" to "noStream", "contentUri" to contentUri),
    )

    /**
     * Purpose: Release a stream whose copy will never run.
     *
     * @param streamId the stream.
     * @param streamInfo its pipes.
     * @param error why the copy will not run.
     * @return [error], with any close failure attached as suppressed.
     * @throws Nothing.
     */
    private fun <E : Throwable> abandon(streamId: String, streamInfo: StreamInfo, error: E): E {
        streamInfo.released.set(true)
        // A concurrent dispose() that claimed the stream first releases it itself.
        if (activeStreams.remove(streamId, streamInfo)) {
            releaseEnds(streamInfo).forEach { error.addSuppressed(it) }
        }
        return error
    }

    /**
     * Purpose: Close a released stream's provider input and both pipe ends,
     * each attempted even when another fails.
     *
     * @param streamInfo the stream's pipes and active input.
     * @return the close failures, in close order; empty when all closed. A
     *   provider stream's close may fail with an unchecked exception, so
     *   every [Exception] is collected, and no failure skips the ends after
     *   it.
     * @throws Nothing.
     */
    private fun releaseEnds(streamInfo: StreamInfo): List<Exception> {
        val closes = listOf(
            { streamInfo.activeInput.getAndSet(null)?.close() },
            { streamInfo.pipe.closeWriteEndOnce() },
            { streamInfo.pipe.readEnd.close() },
        )
        return closes.mapNotNull { close ->
            try {
                close()
                null
            } catch (e: Exception) {
                e
            }
        }
    }

    /**
     * Purpose: Run one stream's copy, then close its write end.
     *
     * Every exit closes the write end, so the reader sees end-of-stream
     * instead of blocking until closeStream (UNFY-300). End-of-stream alone
     * does not mean the copy succeeded: closeStream waits for this copy to
     * finish and raises any failure it recorded (UNFY-365).
     *
     * @param streamInfo the stream's pipes and input.
     * @return Nothing.
     * @throws Error a JVM error from the copy, on the copy's own thread;
     *   other failures are recorded for closeStream.
     */
    private fun runCopy(streamInfo: StreamInfo) {
        // A release that claimed the copy first already closed everything.
        val copy = streamInfo.copy
        if (!copy.claimed.compareAndSet(false, true)) return
        try {
            if (!streamInfo.released.get()) copyContentToPipe(streamInfo)
        } finally {
            try {
                streamInfo.pipe.closeWriteEndOnce()
            } catch (e: Exception) {
                // Only this copy closed the write end here, so a release did
                // not cause this failure; record it even after a release.
                copy.record(e)
            } finally {
                copy.finished.complete(Unit)
            }
        }
    }

    /**
     * Purpose: Copy the provider input openStream opened into the write
     * end, then close the input.
     *
     * Only [Exception]s are recorded. A JVM [Error] propagates on the copy's
     * own thread, after the input is closed. Whether a release caused a copy
     * failure is read the moment the failure is caught.
     *
     * A failure thrown just before the release takes a resource, and caught
     * just after, cannot be told apart from one the release caused, so it is
     * not recorded. Only a reader that closes before end-of-stream meets
     * that window, and it has already given up on the content.
     *
     * @param streamInfo the stream's pipes and input.
     * @return Nothing.
     * @throws Error a JVM error from the copy or the input's close; any
     *   other failure of either is recorded for closeStream.
     */
    private fun copyContentToPipe(streamInfo: StreamInfo) {
        // Null when a release already closed the input openStream opened.
        val input = streamInfo.activeInput.get() ?: return
        try {
            copyChunks(input, streamInfo)
        } catch (e: Exception) {
            // A release takes the input or the write end before closing it,
            // so a failure caught while neither is taken is the copy's own,
            // even after claim() marked the stream released. The release
            // closes the read end last, so a broken pipe on the write end
            // needs no special case, and a provider's own "Broken pipe" is a
            // real copy failure.
            if (!streamInfo.releaseTookAResource()) streamInfo.copy.record(e)
        } finally {
            // The input stays in activeInput during the copy, so a release
            // can close it to unblock a stalled read; whichever takes it out
            // closes it, so a failure here is this copy's own close.
            try {
                streamInfo.activeInput.getAndSet(null)?.close()
            } catch (e: Exception) {
                streamInfo.copy.record(e)
            }
        }
    }

    /**
     * Purpose: Move the provider content into the write end in chunks,
     * stopping between chunks once the stream is released.
     *
     * @param input the provider stream.
     * @param streamInfo the stream's pipe and release state.
     * @return Nothing.
     * @throws IOException when a read or a write fails.
     * @throws RuntimeException when the provider's read fails with one.
     */
    private fun copyChunks(input: InputStream, streamInfo: StreamInfo) {
        // Not closed itself: the write end closes only through
        // Pipe.closeWriteEndOnce, so a release on another thread can close
        // it without a second close here.
        val output = FileOutputStream(streamInfo.pipe.writeEnd.fileDescriptor)
        val buffer = ByteArray(8192)
        while (!streamInfo.released.get()) {
            val bytesRead = input.read(buffer)
            if (bytesRead == -1) break
            output.write(buffer, 0, bytesRead)
        }
    }

    /**
     * Purpose: Hand a released stream's copy failures to the log, because no
     * caller will raise them: after dispose(), or after closeStream stopped
     * waiting.
     *
     * A failure already recorded is logged now, even when the copy is still
     * running; each later one is logged as it is recorded.
     *
     * @param streamId the released stream, named in the log.
     * @param copy the released stream's copy.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logCopyFailuresFromNowOn(streamId: String, copy: CopyState) {
        copy.orphanFailuresTo { Log.w(TAG, "stream $streamId: copy failed with no caller to raise it", it) }
    }

    /**
     * Purpose: Log a closing stream's close failures once its release
     * finishes, because closeStream stopped waiting for it.
     *
     * @param closing the closing stream.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logCloseFailuresWhenReleased(closing: Closing) {
        // Undispatched, so a release that already finished logs at once.
        workers.releases.launch(start = CoroutineStart.UNDISPATCHED) {
            logCloseFailures(closing.streamId, closing.release.await())
        }
    }

    /**
     * Purpose: Log a released stream's close failures that no caller will
     * raise: after dispose(), or after closeStream stopped waiting.
     *
     * @param streamId the stream, named in the log.
     * @param failures the close failures, each logged on its own.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logCloseFailures(streamId: String, failures: List<Exception>) {
        failures.forEach { Log.e(TAG, "stream $streamId: closing it failed with no caller to raise it", it) }
    }
}

/**
 * Purpose: Merge failures into one, so a caller raises or logs a single
 * exception that still carries every cause.
 *
 * @param failures the failures, most informative first.
 * @return the first of [failures] with the rest suppressed onto it, or null
 *   when [failures] is empty.
 * @throws Nothing.
 */
private fun firstWithRestSuppressed(failures: List<Exception>): Exception? {
    val first = failures.firstOrNull() ?: return null
    failures.drop(1).forEach { first.addSuppressed(it) }
    return first
}
