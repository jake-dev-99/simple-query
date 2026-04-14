package io.simplezen.simple_query

import android.content.Context
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding

/**
 * Flutter plugin for universal Android ContentProvider queries.
 *
 * Provides CRUD operations, streaming, and reactive observers for any content provider.
 */
class SimpleQueryPlugin : FlutterPlugin, ActivityAware {

    private var applicationContext: Context? = null
    private var flutterApi: QueryFlutterApi? = null
    private var hostApiImpl: QueryHostApiImpl? = null

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        applicationContext = binding.applicationContext

        flutterApi = QueryFlutterApi(binding.binaryMessenger)

        hostApiImpl = QueryHostApiImpl(
            context = binding.applicationContext,
            flutterApi = flutterApi!!,
        )

        QueryHostApi.setUp(binding.binaryMessenger, hostApiImpl)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        // Clean up all observers and streams
        hostApiImpl?.dispose()
        hostApiImpl = null
        flutterApi = null
        applicationContext = null

        QueryHostApi.setUp(binding.binaryMessenger, null)
    }

    // --- ActivityAware ---

    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        // Activity reference not needed for ContentResolver operations
    }

    override fun onDetachedFromActivityForConfigChanges() {
        // No-op
    }

    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
        // No-op
    }

    override fun onDetachedFromActivity() {
        // Unregister observers to prevent leaks during activity destruction
        hostApiImpl?.unregisterAllObservers()
    }
}
