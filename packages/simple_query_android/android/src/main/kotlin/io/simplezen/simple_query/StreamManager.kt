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
import java.util.concurrent.Semaphore
import java.util.concurrent.atomic.AtomicLong

/**
 * Manages streaming of binary content via named pipes.
 *
 * Uses ParcelFileDescriptor.createPipe() to create a zero-copy streaming
 * mechanism that avoids MethodChannel size limits.
 *
 * Enforces a soft limit of 8 concurrent streams with queue for additional requests.
 *
 * [executor] runs the background copies and [createPipe] makes each
 * stream's pipe pair; both are injectable so tests can observe them.
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
    private val streamSemaphore = Semaphore(MAX_CONCURRENT_STREAMS)
    private val activeStreams = ConcurrentHashMap<String, StreamInfo>()

    private data class StreamInfo(
        val contentUri: String,
        val readPipe: ParcelFileDescriptor,
        val writePipe: ParcelFileDescriptor,
        val pipePath: String,
        val bytesTransferred: AtomicLong = AtomicLong(0),
    )

    /**
     * Open a streaming pipe for reading binary content.
     *
     * The returned StreamDescriptor contains:
     * - streamId: Unique ID for this stream
     * - pipePath: Path to the pipe file for reading (use File(pipePath).inputStream())
     *
     * Call closeStream(streamId) when done to release resources.
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
        activeStreams[streamId] = streamInfo

        // Start background copy with semaphore-based throttling
        executor.execute {
            try {
                // Wait for permit (blocks if at limit)
                streamSemaphore.acquire()

                try {
                    copyContentToPipe(streamInfo, streamId)
                } finally {
                    streamSemaphore.release()
                }
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
                notifyError(streamId, contentUri, "Stream interrupted", 0)
            } finally {
                // Every exit closes the write end, so the reader sees
                // end-of-stream instead of blocking until closeStream (UNFY-300).
                closeWriteEnd(streamId, streamInfo)
            }
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
     * Purpose: Close a stream and release its resources.
     *
     * @param streamId the stream to close; an unknown id is a no-op.
     * @return Nothing.
     * @throws IOException the first failure closing either pipe end, after
     *   both ends were attempted, so a failed close reaches the host
     *   boundary instead of being ignored (UNFY-300).
     */
    fun closeStream(streamId: String) {
        val streamInfo = activeStreams.remove(streamId) ?: return

        var failure: IOException? = null
        for (end in listOf(streamInfo.writePipe, streamInfo.readPipe)) {
            try {
                end.close()
            } catch (e: IOException) {
                failure?.addSuppressed(e) ?: run { failure = e }
            }
        }
        failure?.let { throw it }
    }

    /**
     * Purpose: Close all active streams on plugin detach.
     *
     * No caller remains to act on a failure at detach, so each failed close
     * is logged with its stream id and the rest still close.
     *
     * @return Nothing.
     * @throws Nothing.
     */
    fun closeAll() {
        val ids = activeStreams.keys.toList()
        for (id in ids) {
            try {
                closeStream(id)
            } catch (e: IOException) {
                Log.e(TAG, "closeAll: closing stream $id failed", e)
            }
        }
        executor.shutdown()
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
     * Purpose: Close a stream's write end once its copy has ended. Closing
     * is idempotent, so an end the copy already closed is a no-op.
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

    private fun notifyError(
        streamId: String,
        contentUri: String,
        message: String,
        bytesTransferred: Long
    ) {
        mainHandler.post {
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
