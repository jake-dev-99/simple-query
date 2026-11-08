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
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Manages streaming of binary content via named pipes.
 *
 * Uses ParcelFileDescriptor.createPipe() to create a zero-copy streaming
 * mechanism that avoids MethodChannel size limits.
 *
 * [executor] runs the background copies, and it alone bounds how many run
 * at once: the default pool runs 8 and queues the rest. [createPipe] makes
 * each stream's pipe pair. Both are injectable so tests can observe them.
 *
 * No pipe end is closed twice from two threads (UNFY-300): the write end
 * closes through [closeWriteEndOnce], and only whoever removes a stream
 * from [activeStreams] (closeStream, dispose, or a failed openStream)
 * closes its read end.
 *
 * A copy that fails is not reported on its own: the failure is recorded
 * and raised by closeStream (UNFY-365), so a reader that saw end-of-stream
 * learns from closeStream whether the content is complete. closeStream
 * waits for a running copy to finish first, without blocking a thread, so a
 * failure the copy records while the reader is already closing is still
 * raised. After dispose(), a copy that finishes later logs its own failure.
 */
class StreamManager(
    private val context: Context,
    private val executor: ExecutorService = Executors.newFixedThreadPool(MAX_CONCURRENT_STREAMS),
    private val createPipe: () -> Array<ParcelFileDescriptor> = { ParcelFileDescriptor.createPipe() },
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

    private data class StreamInfo(
        val contentUri: String,
        val readPipe: ParcelFileDescriptor,
        val writePipe: ParcelFileDescriptor,
        val pipePath: String,
        /** Set once the stream is released; its copy then stops, or never starts. */
        val released: AtomicBoolean = AtomicBoolean(false),
        /** Claims the single close of [writePipe]. */
        val writeClosed: AtomicBoolean = AtomicBoolean(false),
        /**
         * The provider stream openStream opened, until the copy or a release
         * closes it; a release closes it to unblock a stalled read or a copy
         * that never runs.
         */
        val activeInput: AtomicReference<InputStream?> = AtomicReference(null),
        /** The copy's first failure, raised by closeStream. */
        val copyFailure: AtomicReference<Exception?> = AtomicReference(null),
        /**
         * Claimed by the copy when it starts, or by a release before it
         * starts; a copy claimed by a release never runs.
         */
        val copyClaimed: AtomicBoolean = AtomicBoolean(false),
        /** Completed when a started copy has finished, including its write-end close. */
        val copyFinished: CompletableDeferred<Unit> = CompletableDeferred(),
        /**
         * Set by dispose(): no caller remains to raise a copy failure, so a
         * copy that finishes later logs it.
         */
        val logFailureOnFinish: AtomicBoolean = AtomicBoolean(false),
    )

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

        val streamInfo = StreamInfo(
            contentUri = contentUri,
            readPipe = readPipe,
            writePipe = writePipe,
            pipePath = pipePath,
        )
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
     *   [COPY_STOP_TIMEOUT_MS] and recorded no failure.
     * @throws IOException when the platform reports a failed close and no
     *   earlier failure applies, after both ends were attempted (UNFY-300).
     * @throws RuntimeException when the provider stream's close fails with
     *   one and no earlier failure applies, after both ends were attempted.
     * @throws kotlinx.coroutines.CancellationException when the calling
     *   coroutine is cancelled while waiting for the copy.
     */
    suspend fun closeStream(streamId: String) {
        val streamInfo = claim(streamId) ?: return
        val closeFailure = releaseEnds(streamInfo)
        val stopFailure = awaitCopy(streamInfo)
        // The copy's own failure explains the content best, then a copy that
        // would not stop, then a failed close.
        val failures = listOfNotNull(streamInfo.copyFailure.get(), stopFailure, closeFailure)
        failures.firstOrNull()?.let { primary ->
            failures.drop(1).forEach { primary.addSuppressed(it) }
            throw primary
        }
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
            releaseEnds(streamInfo)?.let { Log.e(TAG, "dispose: closing stream $id failed", it) }
            // Set before checking the copy, which checks this flag after it
            // finishes, so one of the two logs the failure.
            streamInfo.logFailureOnFinish.set(true)
            if (streamInfo.copyFinished.isCompleted) logCopyFailure(id, streamInfo)
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
     * Purpose: Wait for a released stream's copy to finish, so every failure
     * it records is visible to closeStream.
     *
     * @param streamInfo the released stream.
     * @return null when the copy never started or has finished; otherwise an
     *   [IOException] saying the copy did not stop within
     *   [COPY_STOP_TIMEOUT_MS], so its content may be incomplete.
     * @throws kotlinx.coroutines.CancellationException when the calling
     *   coroutine is cancelled while waiting.
     */
    private suspend fun awaitCopy(streamInfo: StreamInfo): IOException? {
        // Claiming the copy first means it never starts.
        if (streamInfo.copyClaimed.compareAndSet(false, true)) return null
        val finished = withTimeoutOrNull(COPY_STOP_TIMEOUT_MS) { streamInfo.copyFinished.await() }
        if (finished != null) return null
        return IOException(
            "simple_query: the copy of ${streamInfo.contentUri} did not stop within " +
                "${COPY_STOP_TIMEOUT_MS}ms of its release; treat the content as incomplete",
        )
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
            releaseEnds(streamInfo)?.let { error.addSuppressed(it) }
        }
        return error
    }

    /**
     * Purpose: Close a released stream's provider input and both pipe ends,
     * each attempted even when another fails.
     *
     * @param streamInfo the stream's pipes and active input.
     * @return the first close failure, with any later one suppressed onto
     *   it; null when all closed. A provider stream's close may fail with an
     *   unchecked exception, so every [Exception] is collected, and no
     *   failure skips the ends after it.
     * @throws Nothing.
     */
    private fun releaseEnds(streamInfo: StreamInfo): Exception? {
        var failure: Exception? = null
        val closes = listOf(
            { streamInfo.activeInput.getAndSet(null)?.close() },
            { closeWriteEndOnce(streamInfo) },
            { streamInfo.readPipe.close() },
        )
        for (close in closes) {
            try {
                close()
            } catch (e: Exception) {
                failure = failure?.also { it.addSuppressed(e) } ?: e
            }
        }
        return failure
    }

    /**
     * Purpose: Close a stream's write end exactly once, whichever thread
     * (the copy, or a release) gets there first.
     *
     * @param streamInfo the stream's pipes.
     * @return Nothing.
     * @throws IOException when the platform reports a failed close.
     */
    private fun closeWriteEndOnce(streamInfo: StreamInfo) {
        if (streamInfo.writeClosed.compareAndSet(false, true)) streamInfo.writePipe.close()
    }

    /**
     * Purpose: Run one stream's copy, then close its write end.
     *
     * Every exit closes the write end, so the reader sees end-of-stream
     * instead of blocking until closeStream (UNFY-300). End-of-stream alone
     * does not mean the copy succeeded: closeStream waits for this copy to
     * finish and raises any failure it recorded (UNFY-365).
     *
     * @param streamId the stream, named when a failure is logged after
     *   dispose().
     * @param streamInfo the stream's pipes and input.
     * @return Nothing.
     * @throws Error a JVM error from the copy, on the copy's own thread;
     *   other failures are recorded for closeStream.
     */
    private fun runCopy(streamId: String, streamInfo: StreamInfo) {
        // A release that claimed the copy first already closed everything.
        if (!streamInfo.copyClaimed.compareAndSet(false, true)) return
        try {
            if (!streamInfo.released.get()) copyContentToPipe(streamInfo)
        } finally {
            try {
                closeWriteEndOnce(streamInfo)
            } catch (e: IOException) {
                // Only this copy closed the write end here, so a release did
                // not cause this failure; record it even after a release.
                recordFailure(streamInfo, e)
            } finally {
                streamInfo.copyFinished.complete(Unit)
                // dispose() sets the flag before it checks for a finished
                // copy, so one of the two logs the failure.
                if (streamInfo.logFailureOnFinish.get()) logCopyFailure(streamId, streamInfo)
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
        var failure: Exception? = null
        try {
            copyChunks(input, streamInfo)
        } catch (e: Exception) {
            // claim() sets released before a release closes anything, so a
            // failure caught while it is false is the copy's own. A failure
            // after a release is the release itself (its closed input or
            // write end). Only a release closes this manager's read end, so
            // a broken pipe on the write end needs no special case, and a
            // provider's own "Broken pipe" is a real copy failure.
            if (!streamInfo.released.get()) failure = e
        } finally {
            // The input stays in activeInput during the copy, so a release
            // can close it to unblock a stalled read; whichever takes it out
            // closes it, so a failure here is this copy's own close.
            try {
                streamInfo.activeInput.getAndSet(null)?.close()
            } catch (e: Exception) {
                failure = failure?.also { it.addSuppressed(e) } ?: e
            }
        }
        failure?.let { recordFailure(streamInfo, it) }
    }

    /**
     * Purpose: Move the provider content into the write end in chunks,
     * stopping between chunks once the stream is released.
     *
     * @param input the provider stream.
     * @param streamInfo the stream's pipes.
     * @return Nothing.
     * @throws IOException when a read or a write fails.
     * @throws RuntimeException when the provider's read fails with one.
     */
    private fun copyChunks(input: InputStream, streamInfo: StreamInfo) {
        // Not closed itself: the write end closes only through
        // closeWriteEndOnce, so a release on another thread can close it
        // without a second close here.
        val output = FileOutputStream(streamInfo.writePipe.fileDescriptor)
        val buffer = ByteArray(8192)
        while (!streamInfo.released.get()) {
            val bytesRead = input.read(buffer)
            if (bytesRead == -1) break
            output.write(buffer, 0, bytesRead)
        }
    }

    /**
     * Purpose: Log a disposed stream's copy failure, exactly once, since no
     * caller remains to raise it.
     *
     * Whichever of dispose() and the copy takes the failure out logs it.
     *
     * @param streamId the disposed stream.
     * @param streamInfo the disposed stream's state.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun logCopyFailure(streamId: String, streamInfo: StreamInfo) {
        streamInfo.copyFailure.getAndSet(null)?.let { Log.w(TAG, "dispose: stream $streamId copy failed", it) }
    }

    /**
     * Purpose: Record a copy failure for closeStream to raise (UNFY-365).
     *
     * The first failure is kept, and a later one is attached to it as
     * suppressed. Callers decide whether a failure was caused by a release.
     *
     * @param streamInfo the failed stream.
     * @param error the failure, kept as thrown so [HostErrors] classifies it
     *   by type.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun recordFailure(streamInfo: StreamInfo, error: Exception) {
        if (!streamInfo.copyFailure.compareAndSet(null, error)) {
            streamInfo.copyFailure.get()?.addSuppressed(error)
        }
    }
}
