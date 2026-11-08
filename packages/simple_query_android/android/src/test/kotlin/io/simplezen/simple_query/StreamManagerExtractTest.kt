package io.simplezen.simple_query

import io.simplezen.simple_query.StreamManagerFixture.Companion.CONTENT
import io.simplezen.simple_query.StreamManagerFixture.Companion.NO_STREAM_AUTHORITY
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Purpose: Pin that StreamManager.extractToFile copies the content,
 * raises every failure, and never leaks its temp file (UNFY-300).
 *
 * @throws AssertionError from a test whose behavior does not hold.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class StreamManagerExtractTest {
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
     * Purpose: Pin that extractToFile writes the content to the file whose
     *   path it returns.
     *
     * @return Nothing.
     * @throws AssertionError when the file holds other bytes.
     */
    @Test
    fun extractToFileCopiesTheContent() {
        fixture.register(CONTENT) { ByteArrayInputStream(byteArrayOf(4, 5, 6)) }

        val path = checkNotNull(fixture.manager.extractToFile(CONTENT))

        val file = File(path.removePrefix("file://"))
        assertEquals(listOf<Byte>(4, 5, 6), file.readBytes().toList())
        file.delete()
    }

    /**
     * Purpose: Pin that extractToFile returns null for a provider with no
     *   stream and leaves no temp file.
     *
     * @return Nothing.
     * @throws AssertionError when it returns a path or a temp file remains.
     */
    @Test
    fun extractToFileReturnsNullForNoStreamAndLeavesNoTempFile() {
        val before = fixture.tempFileCount()

        assertNull(fixture.manager.extractToFile("content://$NO_STREAM_AUTHORITY/nostream"))
        assertEquals(before, fixture.tempFileCount())
    }

    /**
     * Purpose: Pin that extractToFile raises a failed read and deletes its
     *   temp file.
     *
     * @return Nothing.
     * @throws AssertionError when the failure is swallowed or a temp file
     *   remains.
     */
    @Test
    fun extractToFileRaisesACopyFailureAndLeavesNoTempFile() {
        fixture.register(CONTENT) {
            object : InputStream() {
                /**
                 * Purpose: Fail the extraction's read.
                 *
                 * @return Nothing; it always throws.
                 * @throws IOException on every call.
                 */
                override fun read(): Int = throw IOException("disk read failed")
            }
        }
        val before = fixture.tempFileCount()

        val error = assertFailsWith<IOException> { fixture.manager.extractToFile(CONTENT) }

        assertEquals("disk read failed", error.message)
        assertEquals(before, fixture.tempFileCount())
    }

    /**
     * Purpose: Pin that extractToFile raises a failed provider-stream close
     *   and deletes its temp file.
     *
     * @return Nothing.
     * @throws AssertionError when the failure is swallowed or a temp file
     *   remains.
     */
    @Test
    fun extractToFileLeavesNoTempFileWhenTheProviderStreamFailsToClose() {
        fixture.register(CONTENT) {
            object : ByteArrayInputStream(byteArrayOf(7, 8)) {
                /**
                 * Purpose: Fail the provider stream's close.
                 *
                 * @return Nothing; it always throws.
                 * @throws IOException on every call.
                 */
                override fun close() = throw IOException("provider close failed")
            }
        }
        val before = fixture.tempFileCount()

        val error = assertFailsWith<IOException> { fixture.manager.extractToFile(CONTENT) }

        assertEquals("provider close failed", error.message)
        assertEquals(before, fixture.tempFileCount())
    }
}
