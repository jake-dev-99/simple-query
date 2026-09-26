package io.simplezen.simple_query

import kotlin.test.assertEquals
import kotlin.test.Test

/**
 * Unit tests for [contentChangeTypeFromFlags].
 *
 * These tests exercise the Kotlin-side mapping without requiring an Android
 * device.
 */
class ObserverRegistryTest {
    @Test
    fun mapsInsertFlag() {
        assertEquals(ContentChangeType.INSERT, contentChangeTypeFromFlags(4))
    }

    @Test
    fun mapsUpdateFlag() {
        assertEquals(ContentChangeType.UPDATE, contentChangeTypeFromFlags(8))
    }

    @Test
    fun mapsDeleteFlag() {
        assertEquals(ContentChangeType.DELETE, contentChangeTypeFromFlags(16))
    }

    @Test
    fun mapsUnknownFlag() {
        assertEquals(ContentChangeType.UNKNOWN, contentChangeTypeFromFlags(1))
    }

    @Test
    fun insertTakesPriorityForMixedFlags() {
        assertEquals(ContentChangeType.INSERT, contentChangeTypeFromFlags(12))
    }
}
