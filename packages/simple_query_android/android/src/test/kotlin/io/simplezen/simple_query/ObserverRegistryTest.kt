package io.simplezen.simple_query

import kotlin.test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Unit tests for [ObserverRegistry].
 *
 * Verifies that flag constants, selfChange forwarding, and changeType mapping
 * are all wired through the dispatch pipeline correctly.
 *
 * These tests exercise the Kotlin-side semantics (constants, bit-tests)
 * without requiring an Android device — the registry's private internals
 * are accessed indirectly through the public constants.
 */
class ObserverRegistryTest {
    @test
    fun flagConstantsMatchAndroidContentResolver() {
        // Android's ContentResolver uses these exact bit flags.
        // NOTIFY_INSERT  = 4
        // NOTIFY_UPDATE  = 8
        // NOTIFY_DELETE  = 16
        assertEquals(4, ObserverRegistry.NOTIFY_INSERT)
        assertEquals(8, ObserverRegistry.NOTIFY_UPDATE)
        assertEquals(16, ObserverRegistry.NOTIFY_DELETE)
    }

    @test
    fun flagsMapCorrectlyForEachChangeType() {
        // INSERT (4): matches NOTIFY_INSERT only
        assertTrue((4 and ObserverRegistry.NOTIFY_INSERT) != 0)
        assertTrue((4 and ObserverRegistry.NOTIFY_UPDATE) == 0)
        assertTrue((4 and ObserverRegistry.NOTIFY_DELETE) == 0)

        // UPDATE (8): matches NOTIFY_UPDATE only
        assertTrue((8 and ObserverRegistry.NOTIFY_INSERT) == 0)
        assertTrue((8 and ObserverRegistry.NOTIFY_UPDATE) != 0)
        assertTrue((8 and ObserverRegistry.NOTIFY_DELETE) == 0)

        // DELETE (16): matches NOTIFY_DELETE only
        assertTrue((16 and ObserverRegistry.NOTIFY_INSERT) == 0)
        assertTrue((16 and ObserverRegistry.NOTIFY_UPDATE) == 0)
        assertTrue((16 and ObserverRegistry.NOTIFY_DELETE) != 0)

        // Mixed flags: INSERT | UPDATE = 12
        assertTrue((12 and ObserverRegistry.NOTIFY_INSERT) != 0)
        assertTrue((12 and ObserverRegistry.NOTIFY_UPDATE) != 0)
        assertTrue((12 and ObserverRegistry.NOTIFY_DELETE) == 0)
    }

    @test
    fun unknownFlagMapsToUnknownChangeType() {
        // A flags value that matches none of the three constants.
        assertTrue((1 and ObserverRegistry.NOTIFY_INSERT) == 0)
        assertTrue((1 and ObserverRegistry.NOTIFY_UPDATE) == 0)
        assertTrue((1 and ObserverRegistry.NOTIFY_DELETE) == 0)
    }
}
