package io.simplezen.simple_query

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import io.flutter.plugin.common.BinaryMessenger
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.util.concurrent.AbstractExecutorService
import java.util.concurrent.TimeUnit
import java.util.function.Supplier
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/**
 * Pins StreamManager's failure handling (UNFY-300): a background copy
 * always closes the pipe's write end, a failed close reaches the caller,
 * and extraction neither swallows failures nor leaks its temp file.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerTest {
    /** Runs each copy inline, so a test observes it without waiting. */
    private class DirectExecutor : AbstractExecutorService() {
        override fun execute(command: Runnable) = command.run()
        override fun shutdown() = Unit
        override fun shutdownNow(): MutableList<Runnable> = mutableListOf()
        override fun isShutdown() = false
        override fun isTerminated() = false
        override fun awaitTermination(timeout: Long, unit: TimeUnit) = true
    }

    /** A pipe end whose close fails, as a dying descriptor's can. */
    private class FailingCloseEnd(wrapped: ParcelFileDescriptor) : ParcelFileDescriptor(wrapped) {
        override fun close() = throw IOException("close failed")
    }

    // "nostream" -> the provider hands back no stream.
    class NoStreamProvider : ContentProvider() {
        override fun onCreate() = true
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? = null
        override fun query(
            uri: Uri,
            projection: Array<String>?,
            selection: String?,
            selectionArgs: Array<String>?,
            sortOrder: String?,
        ): Cursor? = null
        override fun getType(uri: Uri): String? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?) = 0
        override fun update(
            uri: Uri,
            values: ContentValues?,
            selection: String?,
            selectionArgs: Array<String>?,
        ) = 0
    }

    private val sentChannels = mutableListOf<String>()
    private val messenger = object : BinaryMessenger {
        override fun send(channel: String, message: ByteBuffer?) {
            sentChannels.add(channel)
        }

        override fun send(
            channel: String,
            message: ByteBuffer?,
            callback: BinaryMessenger.BinaryReply?,
        ) {
            sentChannels.add(channel)
        }

        override fun setMessageHandler(
            channel: String,
            handler: BinaryMessenger.BinaryMessageHandler?,
        ) = Unit
    }

    private val context = RuntimeEnvironment.getApplication()
    private val createdPipes = mutableListOf<Array<ParcelFileDescriptor>>()
    private var wrapWriteEnd: (ParcelFileDescriptor) -> ParcelFileDescriptor = { it }

    private lateinit var manager: StreamManager

    @Before
    fun setUp() {
        Robolectric.buildContentProvider(NoStreamProvider::class.java).create(NO_STREAM_AUTHORITY)
        manager = StreamManager(
            context,
            QueryFlutterApi(messenger),
            Handler(Looper.getMainLooper()),
            executor = DirectExecutor(),
            createPipe = {
                val pipes = ParcelFileDescriptor.createPipe()
                arrayOf(pipes[0], wrapWriteEnd(pipes[1])).also { createdPipes.add(it) }
            },
        )
    }

    @Test
    fun aFailedOpenClosesTheWriteEndAndReportsAStreamError() {
        register(CONTENT) { throw SecurityException("Permission Denial") }

        manager.openStream(CONTENT)
        shadowOf(Looper.getMainLooper()).idle()

        assertFalse(createdPipes.single()[1].fileDescriptor.valid())
        assertEquals(listOf(STREAM_ERROR_CHANNEL), sentChannels)
    }

    @Test
    fun aFileNotFoundOpenClosesTheWriteEnd() {
        // FileNotFoundException is an IOException, so it takes the copy's
        // IOException branch rather than the generic one.
        register(CONTENT) { throw FileNotFoundException("No content provider") }

        manager.openStream(CONTENT)
        shadowOf(Looper.getMainLooper()).idle()

        assertFalse(createdPipes.single()[1].fileDescriptor.valid())
        assertEquals(listOf(STREAM_ERROR_CHANNEL), sentChannels)
    }

    @Test
    fun aSuccessfulCopyClosesTheWriteEndWithoutAnError() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }

        manager.openStream(CONTENT)
        shadowOf(Looper.getMainLooper()).idle()

        assertFalse(createdPipes.single()[1].fileDescriptor.valid())
        assertTrue(sentChannels.isEmpty())
    }

    @Test
    fun closeStreamReportsAFailedCloseAfterClosingTheOtherEnd() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(1)) }
        wrapWriteEnd = { FailingCloseEnd(it) }
        val descriptor = manager.openStream(CONTENT)

        val error = assertFailsWith<IOException> { manager.closeStream(descriptor.streamId) }

        assertEquals("close failed", error.message)
        assertFalse(createdPipes.single()[0].fileDescriptor.valid())
    }

    @Test
    fun extractToFileCopiesTheContent() {
        register(CONTENT) { ByteArrayInputStream(byteArrayOf(4, 5, 6)) }

        val path = checkNotNull(manager.extractToFile(CONTENT))

        val file = File(path.removePrefix("file://"))
        assertEquals(listOf<Byte>(4, 5, 6), file.readBytes().toList())
        file.delete()
    }

    @Test
    fun extractToFileReturnsNullForNoStreamAndLeavesNoTempFile() {
        val before = tempFileCount()

        assertNull(manager.extractToFile("content://$NO_STREAM_AUTHORITY/nostream"))
        assertEquals(before, tempFileCount())
    }

    @Test
    fun extractToFileRaisesACopyFailureAndLeavesNoTempFile() {
        register(CONTENT) {
            object : InputStream() {
                override fun read(): Int = throw IOException("disk read failed")
            }
        }
        val before = tempFileCount()

        val error = assertFailsWith<IOException> { manager.extractToFile(CONTENT) }

        assertEquals("disk read failed", error.message)
        assertEquals(before, tempFileCount())
    }

    private fun register(uri: String, stream: () -> InputStream) {
        shadowOf(context.contentResolver)
            .registerInputStreamSupplier(Uri.parse(uri), Supplier { stream() })
    }

    private fun tempFileCount() =
        context.cacheDir.list()?.count { it.startsWith("sq_extract_") } ?: 0

    private companion object {
        const val CONTENT = "content://io.simplezen.simple_query.streams.test/item"
        const val NO_STREAM_AUTHORITY = "io.simplezen.simple_query.nostream.test"
        const val STREAM_ERROR_CHANNEL =
            "dev.flutter.pigeon.simple_query.QueryFlutterApi.onStreamError"
    }
}
