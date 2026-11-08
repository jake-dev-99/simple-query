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
import kotlinx.coroutines.withTimeoutOrNull

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
 * waits for a running copy to finish first, without blocking a thread, so a
 * failure the copy records while the reader is already closing is still
 * raised. A failure no caller can raise any more (after dispose(), or after
 * closeStream stopped waiting) is logged once instead.
 *
 * @param context the context whose content resolver opens the content.
 * @param executor runs the background copies, and alone bounds how many run
 *   at once: the default pool runs 8 and queues the rest. Injectable so
 *   tests can control the copies.
 * @param createPipe makes each stream's pipe pair; injectable so tests can
 *   observe the ends.
 * @param copyStopTimeoutMs how long closeStream waits for a running copy;
 *   injectable so tests can time it out.
 * @throws Nothing.
 */
class StreamManager(
    private val context: Context,
    private val executor: ExecutorService = Executors.newFixedThreadPool(MAX_CONCURRENT_STREAMS),
    private val createPipe: () -> Array<ParcelFileDescriptor> = { ParcelFileDescriptor.createPipe() },
    private val copyStopTimeoutMs: Long = COPY_STOP_TIMEOUT_MS,
) {
    companion object {
        private const val MAX_CONCURRENT_STREAMS = 8
        private const val TAG = "StreamManager"

        /**
         * How long closeStream waits for a running copy to stop once both
         * pipe ends and the provider input are closed.
         */
        private const val COPY_STOP_TIMEOUT_MS = 5_000L
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

        /** Completed when a started copy has finished, including its write-end close. */
        val finished = CompletableDeferred<Unit>()

        /** Set once no caller will raise the failure, so the copy logs it when it finishes. */
        val logFailureOnFinish = AtomicBoolean(false)

        private val failures = mutableListOf<Exception>()

        /**
         * Purpose: Record one copy failure, in the order it happened.
         *
         * @param error the failure, kept as thrown so [HostErrors] classifies
         *   it by type.
         * @return Nothing.
         * @throws Nothing.
         */
        @Synchronized
        fun record(error: Exception) {
            failures.add(error)
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
            executor.execute { runCopy(streamId, streamInfo) }
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
     * for a running copy to finish, and raise a failure the copy recorded
     * (UNFY-365).
     *
     * Closing the write end unblocks a copy stuck writing to a reader that
     * stopped reading; closing the provider stream unblocks one stuck in a
     * stalled provider read. A copy this release stops raises nothing.
     * Waiting for the copy means a failure it records after the reader saw
     * end-of-stream, such as a failed write-end close, is still raised. The
     * wait suspends; it does not block the calling thread.
     *
     * @param streamId the stream to release; an unknown id is a no-op.
     * @return Nothing.
     * @throws Exception the copy's recorded failure, after both ends were
     *   attempted, with any later failure attached as suppressed.
     * @throws IOException when the copy did not stop within
     *   [copyStopTimeoutMs] and recorded no failure; a failure it records
     *   later is logged.
     * @throws IOException when the platform reports a failed close and no
     *   earlier failure applies, after both ends were attempted (UNFY-300).
     * @throws RuntimeException when the provider stream's close fails with
     *   one and no earlier failure applies, after both ends were attempted.
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting for the copy; a failure the copy records later is
     *   logged.
     */
    suspend fun closeStream(streamId: String) {
        val streamInfo = claim(streamId) ?: return
        val closeFailures = releaseEnds(streamInfo)
        // The copy's own failure explains the content best, then a copy that
        // would not stop, then a failed close.
        raiseFirst(copyFailures(streamId, streamInfo) + closeFailures)
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
            releaseEnds(streamInfo).forEach { Log.e(TAG, "dispose: closing stream $id failed", it) }
            logFailureWhenFinished(id, streamInfo)
        }
        executor.shutdown()
    }

    /**
     * Purpose: Mark a stream released and take it out of [activeStreams], so
     * exactly one caller releases its ends.
     *
     * The stream is marked before it is claimed, so a copy starting in
     * between skips its work, and a failure the copy raises from then on is
     * the release itself and is not recorded.
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
     * Purpose: Collect the failures a released stream's copy leaves for
     * closeStream to raise, after waiting for the copy to stop.
     *
     * When the copy does not stop in time, the failures recorded so far are
     * taken first, and only then is the rest handed to the log. Handing off
     * first would let a copy that finishes in between log a failure this
     * caller can still raise.
     *
     * @param streamId the released stream.
     * @param streamInfo the released stream's state.
     * @return the copy's failure, if any, then an [IOException] saying the
     *   copy did not stop within [copyStopTimeoutMs], when it did not.
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting.
     */
    private suspend fun copyFailures(streamId: String, streamInfo: StreamInfo): List<Exception> {
        val stopped = awaitCopy(streamId, streamInfo)
        val copyFailure = streamInfo.copy.takeFailure()
        if (stopped) return listOfNotNull(copyFailure)
        logFailureWhenFinished(streamId, streamInfo)
        return listOfNotNull(
            copyFailure,
            IOException(
                "simple_query: the copy of ${streamInfo.contentUri} did not stop within " +
                    "${copyStopTimeoutMs}ms of its release; treat the content as incomplete",
            ),
        )
    }

    /**
     * Purpose: Wait for a released stream's copy to finish, so every failure
     * it records is visible to closeStream.
     *
     * A cancelled wait leaves no caller to raise a failure the copy records
     * afterwards, so that failure is handed to the log.
     *
     * @param streamId the released stream.
     * @param streamInfo the released stream's state.
     * @return true when the copy never started or has finished; false when
     *   it did not stop within [copyStopTimeoutMs].
     * @throws CancellationException when the calling coroutine is cancelled
     *   while waiting.
     */
    private suspend fun awaitCopy(streamId: String, streamInfo: StreamInfo): Boolean {
        val copy = streamInfo.copy
        // Claiming the copy first means it never starts.
        if (copy.claimed.compareAndSet(false, true)) return true
        try {
            return withTimeoutOrNull(copyStopTimeoutMs) { copy.finished.await() } != null
        } catch (e: CancellationException) {
            logFailureWhenFinished(streamId, streamInfo)
            throw e
        }
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
     * @param streamId the stream, named when its failure is logged.
     * @param streamInfo the stream's pipes and input.
     * @return Nothing.
     * @throws Error a JVM error from the copy, on the copy's own thread;
     *   other failures are recorded for closeStream.
     */
    private fun runCopy(streamId: String, streamInfo: StreamInfo) {
        // A release that claimed the copy first already closed everything.
        val copy = streamInfo.copy
        if (!copy.claimed.compareAndSet(false, true)) return
        try {
            if (!streamInfo.released.get()) copyContentToPipe(streamInfo)
        } finally {
            try {
                streamInfo.pipe.closeWriteEndOnce()
            } catch (e: IOException) {
                // Only this copy closed the write end here, so a release did
                // not cause this failure; record it even after a release.
                copy.record(e)
            } finally {
                copy.finished.complete(Unit)
                // logFailureWhenFinished sets the flag before it checks for a
                // finished copy, so one of the two logs the failure.
                if (copy.logFailureOnFinish.get()) logCopyFailure(streamId, copy)
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
            // claim() sets released before a release closes anything, so a
            // failure caught while it is false is the copy's own. A failure
            // after a release is the release itself (its closed input or
            // write end). Only a release closes this manager's read end, so
            // a broken pipe on the write end needs no special case, and a
            // provider's own "Broken pipe" is a real copy failure.
            if (!streamInfo.released.get()) streamInfo.copy.record(e)
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
     * Purpose: Hand a released stream's copy failure to the log, because no
     * caller will raise it: after dispose(), or after closeStream stopped
     * waiting.
     *
     * The flag is set before the copy is checked, and the copy checks it
     * after it finishes, so one of the two logs the failure.
     *
     * @param streamId the released stream.
     * @param streamInfo the released stream's state.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logFailureWhenFinished(streamId: String, streamInfo: StreamInfo) {
        val copy = streamInfo.copy
        copy.logFailureOnFinish.set(true)
        if (copy.finished.isCompleted) logCopyFailure(streamId, copy)
    }

    /**
     * Purpose: Log a copy failure that no caller will raise, exactly once.
     *
     * @param streamId the stream, named in the log.
     * @param copy the stream's copy.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logCopyFailure(streamId: String, copy: CopyState) {
        copy.takeFailure()?.let { Log.w(TAG, "stream $streamId: copy failed with no caller to raise it", it) }
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
