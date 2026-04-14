package io.simplezen.simple_query

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
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
 */
class StreamManager(
    private val context: Context,
    private val flutterApi: QueryFlutterApi,
    private val mainHandler: Handler,
) {
    companion object {
        private const val MAX_CONCURRENT_STREAMS = 8
        private const val TAG = "StreamManager"
    }

    private val contentResolver get() = context.contentResolver
    private val streamSemaphore = Semaphore(MAX_CONCURRENT_STREAMS)
    private val executor = Executors.newFixedThreadPool(MAX_CONCURRENT_STREAMS)
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
        val pipes = ParcelFileDescriptor.createPipe()
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
        executor.submit {
            try {
                // Wait for permit (blocks if at limit)
                streamSemaphore.acquire()

                try {
                    copyContentToPipe(streamInfo, streamId)
                } finally {
                    streamSemaphore.release()
                }
            } catch (e: InterruptedException) {
                notifyError(streamId, contentUri, "Stream interrupted", 0)
            }
        }

        return StreamDescriptor(
            streamId = streamId,
            pipePath = pipePath,
        )
    }

    /**
     * Extract content to a temporary file.
     *
     * Returns the file:// URI to the temp file.
     * Caller is responsible for deleting the file when done.
     */
    fun extractToFile(contentUri: String): String? {
        val uri = Uri.parse(contentUri)

        return try {
            val tempFile = File.createTempFile("sq_extract_", null, context.cacheDir)

            contentResolver.openInputStream(uri)?.use { input ->
                FileOutputStream(tempFile).use { output ->
                    input.copyTo(output)
                }
            } ?: return null

            "file://${tempFile.absolutePath}"
        } catch (e: Exception) {
            null
        }
    }

    /**
     * Close a stream and release its resources.
     */
    fun closeStream(streamId: String) {
        val streamInfo = activeStreams.remove(streamId) ?: return

        try {
            streamInfo.writePipe.close()
        } catch (e: Exception) {
            // Ignore close errors on write side
        }

        try {
            streamInfo.readPipe.close()
        } catch (e: Exception) {
            // Ignore close errors on read side
        }
    }

    /**
     * Close all active streams. Called on plugin dispose.
     */
    fun closeAll() {
        val ids = activeStreams.keys.toList()
        for (id in ids) {
            closeStream(id)
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
        } catch (e: Exception) {
            notifyError(
                streamId,
                streamInfo.contentUri,
                "Stream error: ${e.message}",
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
