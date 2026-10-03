package io.simplezen.simple_query

import android.content.ContentResolver
import android.database.ContentObserver
import android.net.Uri
import android.os.Handler
import android.os.Looper
import io.flutter.plugin.common.StandardMessageCodec
import io.flutter.plugin.common.BinaryMessenger
import java.nio.ByteBuffer
import kotlin.test.Test
import kotlin.test.assertEquals
import org.junit.After
import org.junit.Before
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class ObserverDispatchTest {
    // Decode the native-to-Dart payload. The generated Kotlin decoder is for
    // Dart-to-native messages, whose Pigeon codec writes all integers as int64.
    private object FlutterObserverCodec : StandardMessageCodec() {
        override fun readValueOfType(type: Byte, buffer: ByteBuffer): Any? = when (type) {
            129.toByte() -> ContentChangeType.ofRaw((readValue(buffer) as Number).toInt())
            146.toByte() -> readValue(buffer)
            else -> super.readValueOfType(type, buffer)
        }
    }

    private val events = mutableListOf<List<Any?>>()
    private lateinit var registry: ObserverRegistry
    private lateinit var observer: ContentObserver
    private lateinit var observerId: String

    @Before
    fun registerObserver() {
        val context = RuntimeEnvironment.getApplication()
        val messenger = object : BinaryMessenger {
            override fun send(channel: String, message: ByteBuffer?) {
                send(channel, message, null)
            }

            override fun send(
                channel: String,
                message: ByteBuffer?,
                callback: BinaryMessenger.BinaryReply?,
            ) {
                assertEquals(
                    "dev.flutter.pigeon.simple_query.QueryFlutterApi.onContentChange",
                    channel,
                )
                message!!.flip()
                val args = FlutterObserverCodec.decodeMessage(message) as List<*>
                @Suppress("UNCHECKED_CAST")
                events.add(args.single() as List<Any?>)
            }

            override fun setMessageHandler(
                channel: String,
                handler: BinaryMessenger.BinaryMessageHandler?,
            ) = Unit
        }
        registry = ObserverRegistry(context, QueryFlutterApi(messenger), Handler(Looper.getMainLooper()))
        observerId = registry.register("content://sms", true)
        observer = shadowOf(context.contentResolver)
            .getContentObservers(Uri.parse("content://sms")).single()
    }

    @After
    fun unregisterObserver() {
        registry.unregisterAll()
    }

    @Test
    fun legacyCallbackPreservesSelfChangeAndFallsBackToRegisteredUri() {
        observer.onChange(true)
        assertEquals(
            listOf(listOf<Any?>(observerId, "content://sms", ContentChangeType.UNKNOWN, null, true)),
            events,
        )
    }

    @Test
    fun uriCallbackPreservesChangedUriAndFalseSelfChange() {
        observer.onChange(false, Uri.parse("content://sms/42"))
        assertEquals(
            listOf(listOf<Any?>(observerId, "content://sms/42", ContentChangeType.UNKNOWN, null, false)),
            events,
        )
    }

    @Test
    fun nullUriCallbackPreservesSelfChangeAndFallsBackToRegisteredUri() {
        observer.onChange(true, null as Uri?)
        assertEquals(
            listOf(listOf<Any?>(observerId, "content://sms", ContentChangeType.UNKNOWN, null, true)),
            events,
        )
    }

    @Test
    fun flagsCallbackPreservesRawFlagsAndUsesAndroidNotificationConstants() {
        val flags = ContentResolver.NOTIFY_UPDATE or ContentResolver.NOTIFY_SYNC_TO_NETWORK
        observer.onChange(true, Uri.parse("content://sms/42"), flags)
        assertEquals(
            listOf(listOf<Any?>(observerId, "content://sms/42", ContentChangeType.UPDATE, flags.toLong(), true)),
            events,
        )
    }

    @Test
    fun mixedOperationCallbacksPreserveFlagsWithoutClaimingASingleMutation() {
        val operationFlags = listOf(
            ContentResolver.NOTIFY_INSERT or ContentResolver.NOTIFY_UPDATE,
            ContentResolver.NOTIFY_INSERT or ContentResolver.NOTIFY_DELETE,
            ContentResolver.NOTIFY_UPDATE or ContentResolver.NOTIFY_DELETE,
            ContentResolver.NOTIFY_INSERT or ContentResolver.NOTIFY_UPDATE or ContentResolver.NOTIFY_DELETE,
        )
        for (operations in operationFlags) {
            for (modifiers in 0..3) {
                val flags = operations or modifiers
                observer.onChange(true, Uri.parse("content://sms/42"), flags)
                observer.onChange(false, listOf(Uri.parse("content://sms/43")), flags)
                assertEquals(
                    listOf(
                        listOf<Any?>(observerId, "content://sms/42", ContentChangeType.UNKNOWN, flags.toLong(), true),
                        listOf<Any?>(observerId, "content://sms/43", ContentChangeType.UNKNOWN, flags.toLong(), false),
                    ),
                    events,
                    "flags=$flags",
                )
                events.clear()
            }
        }
    }

    @Test
    fun collectionCallbackEmitsEachUriOnceWithAllMetadata() {
        observer.onChange(false, listOf(Uri.parse("content://sms/42"), Uri.parse("content://sms/43")), ContentResolver.NOTIFY_DELETE)
        assertEquals(
            listOf(
                listOf<Any?>(observerId, "content://sms/42", ContentChangeType.DELETE, 16L, false),
                listOf<Any?>(observerId, "content://sms/43", ContentChangeType.DELETE, 16L, false),
            ),
            events,
        )
    }
}
