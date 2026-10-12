package io.simplezen.simple_query

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

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
 */
class StreamManager(
    private val context: Context,
    private val flutterApi: QueryFlutterApi,
    private val mainHandler: Handler,
    private val executor: ExecutorService = Executors.newFixedThreadPool(MAX_CONCURRENT_STREAMS),
    private val createPipe: () -> Array<ParcelFileDescriptor> = { ParcelFileDescriptor.createPipe() },
) {
    companion object {
        private const val MAX_CONCURRENT_STREAMS = 8
        private const val TAG = "StreamManager"
    }

    private val contentResolver get() = context.contentResolver
    private val activeStreams = ConcurrentHashMap<String, StreamInfo>()
    private val disposed = AtomicBoolean(false)

    private data class StreamInfo(
        val contentUri: String,
        val readPipe: ParcelFileDescriptor,
        val writePipe: ParcelFileDescriptor,
        val pipePath: String,
        val bytesTransferred: AtomicLong = AtomicLong(0),
        /** Set once the stream is released; its copy then stops, or never starts. */
        val released: AtomicBoolean = AtomicBoolean(false),
        /** Claims the single close of [writePipe]. */
        val writeClosed: AtomicBoolean = AtomicBoolean(false),
    )

    /**
     * Purpose: Open a streaming pipe for reading binary content.
     *
     * The copy runs in the background; read the content from the returned
     * pipePath (`File(pipePath).inputStream()`) and call
     * closeStream(streamId) when done.
     *
     * @param contentUri the content to stream.
     * @return the stream's id and pipe path.
     * @throws IllegalStateException when this manager is disposed.
     * @throws RejectedExecutionException when the executor refuses the
     *   copy. Either way the stream's pipes are released before throwing.
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

        if (disposed.get()) {
            throw abandon(
                streamId,
                streamInfo,
                IllegalStateException("simple_query: stream manager is disposed"),
            )
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
        return input.use {
            val tempFile = File.createTempFile("sq_extract_", null, context.cacheDir)
            var extracted = false
            try {
                FileOutputStream(tempFile).use { output -> it.copyTo(output) }
                extracted = true
                "file://${tempFile.absolutePath}"
            } finally {
                if (!extracted) tempFile.delete()
            }
        }
    }

    /**
     * Purpose: Release a stream: stop its copy and close both pipe ends.
     *
     * Closing the write end also unblocks a copy stuck writing to a reader
     * that stopped reading.
     *
     * @param streamId the stream to release; an unknown id is a no-op.
     * @return Nothing.
     * @throws IOException when the platform reports a failed close, after
     *   both ends were attempted (UNFY-300).
     */
    fun closeStream(streamId: String) {
        val streamInfo = activeStreams[streamId] ?: return
        // Marked before it is claimed, so a copy starting in between skips its work.
        streamInfo.released.set(true)
        if (!activeStreams.remove(streamId, streamInfo)) return
        releaseEnds(streamInfo)?.let { throw it }
    }

    /**
     * Purpose: Release every stream on plugin detach, and refuse new ones.
     *
     * Stream errors raised after this are logged, not sent to the detached
     * engine. No caller remains to act on a failed close, so each is logged
     * with its stream id and the rest still close.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    fun dispose() {
        disposed.set(true)
        for (id in activeStreams.keys.toList()) {
            try {
                closeStream(id)
            } catch (e: IOException) {
                Log.e(TAG, "dispose: closing stream $id failed", e)
            }
        }
        executor.shutdown()
    }

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
            releaseEnds(streamInfo)?.let(error::addSuppressed)
        }
        return error
    }

    /**
     * Purpose: Close both pipe ends of a released stream, each attempted
     * even when the other fails.
     *
     * @param streamInfo the stream's pipes.
     * @return the first close failure, with any later one suppressed onto
     *   it; null when both closed.
     * @throws Nothing.
     */
    private fun releaseEnds(streamInfo: StreamInfo): IOException? {
        var failure: IOException? = null
        for (close in listOf({ closeWriteEndOnce(streamInfo) }, { streamInfo.readPipe.close() })) {
            try {
                close()
            } catch (e: IOException) {
                failure?.addSuppressed(e) ?: run { failure = e }
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
     * does not mean the copy succeeded: a failure is reported separately,
     * as a stream error.
     *
     * @param streamId the stream.
     * @param streamInfo its pipes and progress.
     * @return Nothing.
     * @throws Nothing; failures are reported as stream errors.
     */
    private fun runCopy(streamId: String, streamInfo: StreamInfo) {
        try {
            if (!streamInfo.released.get()) copyContentToPipe(streamInfo, streamId)
        } finally {
            try {
                closeWriteEndOnce(streamInfo)
            } catch (e: IOException) {
                notifyError(streamId, streamInfo, "Stream close failed: ${e.message}")
            }
        }
    }

    private fun copyContentToPipe(streamInfo: StreamInfo, streamId: String) {
        val uri = Uri.parse(streamInfo.contentUri)

        try {
            contentResolver.openInputStream(uri)?.use { input ->
                // Not closed itself: the write end closes only through
                // closeWriteEndOnce, so a release on another thread can close
                // it without a second close here.
                val output = FileOutputStream(streamInfo.writePipe.fileDescriptor)
                val buffer = ByteArray(8192)

                // A release stops the copy between chunks.
                while (!streamInfo.released.get()) {
                    val bytesRead = input.read(buffer)
                    if (bytesRead == -1) break
                    output.write(buffer, 0, bytesRead)
                    streamInfo.bytesTransferred.addAndGet(bytesRead.toLong())
                }
            } ?: run {
                notifyError(streamId, streamInfo, "Failed to open input stream for URI")
            }
        } catch (e: IOException) {
            // Don't report broken pipe - it usually means reader closed early (expected)
            if (!e.message.orEmpty().contains("Broken pipe", ignoreCase = true)) {
                notifyError(streamId, streamInfo, "Stream copy failed: ${e.message}")
            }
        } catch (e: Throwable) {
            notifyError(streamId, streamInfo, "Stream error: ${e.message}")
        }
    }

    /**
     * Purpose: Report a stream failure to Dart.
     *
     * A failure of a released stream is the release itself (e.g. its closed
     * write end), so it is not reported. Once disposed, failures are logged
     * instead; that check runs on the main thread, as dispose() does, so no
     * error reaches a detached engine.
     *
     * @param streamId the failed stream.
     * @param streamInfo its URI and progress.
     * @param message what failed.
     * @return Nothing.
     * @throws Nothing.
     */
    private fun notifyError(streamId: String, streamInfo: StreamInfo, message: String) {
        if (streamInfo.released.get()) return
        val bytesTransferred = streamInfo.bytesTransferred.get()
        mainHandler.post {
            if (disposed.get()) {
                Log.w(TAG, "Stream $streamId failed after dispose: $message")
                return@post
            }
            flutterApi.onStreamError(
                StreamError(
                    streamId = streamId,
                    contentUri = streamInfo.contentUri,
                    message = message,
                    bytesTransferred = bytesTransferred,
                )
            ) { /* ignore callback result */ }
        }
    }
}
