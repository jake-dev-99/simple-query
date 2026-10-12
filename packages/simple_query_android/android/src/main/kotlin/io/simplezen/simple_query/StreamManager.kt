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
 * [executor] runs the background copies and bounds how many run at once:
 * the default pool runs 8 and queues the rest. [createPipe] makes each
 * stream's pipe pair. Both are injectable so tests can observe them.
 *
 * Each pipe end has exactly one closer, so no descriptor is closed twice
 * from two threads (UNFY-300): the copy task closes the write end when it
 * ends, and whoever removes the stream from [activeStreams] (closeStream,
 * dispose, or a failed openStream) closes the read end.
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
        /** Set once the reader released the stream; a queued copy then skips its work. */
        val released: AtomicBoolean = AtomicBoolean(false),
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

        try {
            check(!disposed.get()) { "simple_query: stream manager is disposed" }
            executor.execute { runCopy(streamId, streamInfo) }
        } catch (e: IllegalStateException) {
            throw abandon(streamId, streamInfo, e)
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
     * Purpose: Release a stream: close its read end and let its copy stop.
     *
     * The write end belongs to the copy, which closes it when it ends; a
     * copy still queued skips its work.
     *
     * @param streamId the stream to release; an unknown id is a no-op.
     * @return Nothing.
     * @throws IOException when the platform reports a failed close of the
     *   read end (UNFY-300).
     */
    fun closeStream(streamId: String) {
        val streamInfo = activeStreams.remove(streamId) ?: return
        streamInfo.released.set(true)
        streamInfo.readPipe.close()
    }

    /**
     * Purpose: Release every stream on plugin detach, and refuse new ones.
     *
     * Copies still queued run only to close their write ends; stream errors
     * raised after this are logged, not sent to the detached engine. No
     * caller remains to act on a failed close, so each is logged with its
     * stream id and the rest still close.
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
     * @param streamInfo its pipes; both are closed here, since no copy owns
     *   the write end.
     * @param error why the copy will not run.
     * @return [error], with any close failure attached as suppressed.
     * @throws Nothing.
     */
    private fun <E : Throwable> abandon(streamId: String, streamInfo: StreamInfo, error: E): E {
        try {
            streamInfo.writePipe.close()
            closeStream(streamId)
        } catch (closeFailure: IOException) {
            error.addSuppressed(closeFailure)
        }
        return error
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
            closeWriteEnd(streamId, streamInfo)
        }
    }

    private fun copyContentToPipe(streamInfo: StreamInfo, streamId: String) {
        val uri = Uri.parse(streamInfo.contentUri)

        try {
            contentResolver.openInputStream(uri)?.use { input ->
                ParcelFileDescriptor.AutoCloseOutputStream(streamInfo.writePipe).use { output ->
                    val buffer = ByteArray(8192)
                    var bytesRead: Int

                    while (input.read(buffer).also { bytesRead = it } != -1) {
                        output.write(buffer, 0, bytesRead)
                        streamInfo.bytesTransferred.addAndGet(bytesRead.toLong())
                    }
                }
            } ?: run {
                notifyError(
                    streamId,
                    streamInfo.contentUri,
                    "Failed to open input stream for URI",
                    streamInfo.bytesTransferred.get()
                )
            }
        } catch (e: IOException) {
            // Don't report broken pipe - it usually means reader closed early (expected)
            if (!e.message.orEmpty().contains("Broken pipe", ignoreCase = true)) {
                notifyError(
                    streamId,
                    streamInfo.contentUri,
                    "Stream copy failed: ${e.message}",
                    streamInfo.bytesTransferred.get()
                )
            }
        } catch (e: Throwable) {
            notifyError(
                streamId,
                streamInfo.contentUri,
                "Stream error: ${e.message}",
                streamInfo.bytesTransferred.get()
            )
        }
    }

    /**
     * Purpose: Close a stream's write end once its copy has ended. Only the
     * copy's own thread calls this, and closing is idempotent on one
     * thread, so an end the copy's output stream already closed is a no-op.
     *
     * @param streamId the stream whose copy ended.
     * @param streamInfo the stream's pipes.
     * @return Nothing.
     * @throws Nothing; a failed close is reported as a stream error.
     */
    private fun closeWriteEnd(streamId: String, streamInfo: StreamInfo) {
        try {
            streamInfo.writePipe.close()
        } catch (e: IOException) {
            notifyError(
                streamId,
                streamInfo.contentUri,
                "Stream close failed: ${e.message}",
                streamInfo.bytesTransferred.get()
            )
        }
    }

    /**
     * Purpose: Report a stream failure to Dart, or log it once disposed.
     *
     * The disposed check runs on the main thread, as dispose() does, so no
     * error reaches a detached engine.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    private fun notifyError(
        streamId: String,
        contentUri: String,
        message: String,
        bytesTransferred: Long
    ) {
        mainHandler.post {
            if (disposed.get()) {
                Log.w(TAG, "Stream $streamId failed after dispose: $message")
                return@post
            }
            flutterApi.onStreamError(
                StreamError(
                    streamId = streamId,
                    contentUri = contentUri,
                    message = message,
                    bytesTransferred = bytesTransferred,
                )
            ) { /* ignore callback result */ }
        }
    }
}
