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
    fun mixedOperationFlagsRemainUnknown() {
        for (flags in listOf(12, 20, 24, 28)) {
            assertEquals(ContentChangeType.UNKNOWN, contentChangeTypeFromFlags(flags), "flags=$flags")
        }
    }

    @Test
    fun nonOperationFlagsDoNotChangeASingleOperation() {
        for ((operation, expected) in listOf(
            4 to ContentChangeType.INSERT,
            8 to ContentChangeType.UPDATE,
            16 to ContentChangeType.DELETE,
        )) {
            for (modifiers in 0..3) {
                assertEquals(expected, contentChangeTypeFromFlags(operation or modifiers))
            }
        }
    }
}
