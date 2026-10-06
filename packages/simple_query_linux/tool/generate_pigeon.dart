import 'dart:convert';
import 'dart:io';

import 'package:dart_style/dart_style.dart';

const _pigeonVersion = '22.7.4';

/// Purpose: Replace one pinned generator fragment and fail closed on drift.
///
/// @param source is the resolved Pigeon generator source.
/// @param before is the exact upstream fragment expected once.
/// @param after is the corrected fragment written to the temporary copy.
/// @param description identifies the repair when validation fails.
/// @returns The source with exactly one replacement.
/// @throws StateError when the pinned upstream source no longer matches.
String _replaceExactlyOnce(
  String source,
  String before,
  String after,
  String description,
) {
  final matches = before.allMatches(source).length;
  if (matches != 1) {
    throw StateError(
      'Expected one $description fragment in Pigeon $_pigeonVersion, '
      'found $matches. Review the generator patch before regenerating.',
    );
  }
  return source.replaceFirst(before, after);
}

/// Purpose: Copy the resolved Pigeon package without mutating the pub cache.
///
/// @param source is the package-cache directory resolved by package_config.
/// @param destination is the isolated package root used for generation.
/// @returns A future that completes after every file has been copied.
/// @throws FileSystemException when the source cannot be read or copied.
Future<void> _copyDirectory(Directory source, Directory destination) async {
  await destination.create(recursive: true);
  await for (final entity in source.list(recursive: true, followLinks: false)) {
    final relativePath = entity.path.substring(source.path.length + 1);
    final targetPath = '${destination.path}/$relativePath';
    if (entity is Directory) {
      await Directory(targetPath).create(recursive: true);
    } else if (entity is File) {
      await File(targetPath).parent.create(recursive: true);
      await entity.copy(targetPath);
    } else if (entity is Link) {
      await Link(targetPath).create(await entity.target());
    }
  }
}

/// Purpose: Finish generated FlutterApi sends at the messenger boundary.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose callback finishes and decodes each transport result.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterApiCallback(String source) {
  return _replaceExactlyOnce(
    source,
    r'''      indent.writeScoped(
          'static void ${methodPrefix}_${methodName}_cb(GObject* object, GAsyncResult* result, gpointer user_data) {',
          '}', () {
        indent.writeln('GTask* task = G_TASK(user_data);');
        indent.writeln('g_task_return_pointer(task, result, g_object_unref);');
      });''',
    r'''      indent.writeln(
          '/** Purpose: Finish one generated FlutterApi transport send. @param object is the messenger. @param result is its asynchronous response. @param user_data owns the forwarding task. @returns Nothing. @throws Nothing. */');
      indent.writeScoped(
          'static void ${methodPrefix}_${methodName}_cb(GObject* object, GAsyncResult* result, gpointer user_data) {',
          '}', () {
        indent.writeln('g_autoptr(GTask) task = G_TASK(user_data);');
        indent.writeScoped('if (G_IS_TASK(result)) {', '}', () {
          indent.writeln(
              '// Transport finish owns cleanup and preserves explicit cancellation errors.');
          indent.writeln(
              '// Disable only implicit GTask cancellation so finish always runs.');
          indent.writeln(
              'g_task_set_check_cancellable(G_TASK(result), FALSE);');
        });
        indent.writeln('g_autoptr(GError) error = nullptr;');
        indent.writeln(
            'g_autoptr(GBytes) response_bytes = fl_binary_messenger_send_on_channel_finish(FL_BINARY_MESSENGER(object), result, &error);');
        indent.writeScoped('if (response_bytes == nullptr) {', '}', () {
          indent.writeScoped('if (error == nullptr) {', '}', () {
            indent.writeln(
                'error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Flutter message transport returned no response");');
          });
          indent.writeln(
              'g_task_return_error(task, g_steal_pointer(&error));');
          indent.writeln('return;');
        });
        indent.writeln(
            'g_autoptr($codecClassName) codec = ${codecMethodPrefix}_new();');
        indent.writeln(
            'FlValue* response = fl_message_codec_decode_message(FL_MESSAGE_CODEC(codec), response_bytes, &error);');
        indent.writeScoped('if (response == nullptr) {', '}', () {
          indent.writeScoped('if (error == nullptr) {', '}', () {
            indent.writeln(
                'error = g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Flutter message transport returned an invalid response");');
          });
          indent.writeln(
              'g_task_return_error(task, g_steal_pointer(&error));');
          indent.writeln('return;');
        });
        indent.writeln(
            'g_task_return_pointer(task, response, reinterpret_cast<GDestroyNotify>(fl_value_unref));');
      });''',
    'FlutterApi transport callback',
  );
}

/// Purpose: Encode generated FlutterApi calls directly through the messenger.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose send path owns one explicit forwarding task.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterApiSend(String source) {
  return _replaceExactlyOnce(
    source,
    r'''        indent.writeln(
            'FlBasicMessageChannel* channel = fl_basic_message_channel_new(self->messenger, channel_name, FL_MESSAGE_CODEC(codec));');
        indent.writeln(
            'GTask* task = g_task_new(self, cancellable, callback, user_data);');
        indent.writeln('g_task_set_task_data(task, channel, g_object_unref);');
        indent.writeln(
            'fl_basic_message_channel_send(channel, args, cancellable, ${methodPrefix}_${methodName}_cb, task);');''',
    r'''        indent.writeln('g_autoptr(GError) error = nullptr;');
        indent.writeln(
            'g_autoptr(GBytes) message = fl_message_codec_encode_message(FL_MESSAGE_CODEC(codec), args, &error);');
        indent.writeln(
            'g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);');
        indent.writeln(
            '// Forward explicit transport outcomes instead of bypassing transport finish.');
        indent.writeln('g_task_set_check_cancellable(task, FALSE);');
        indent.writeScoped('if (message == nullptr) {', '}', () {
          indent.writeScoped('if (error == nullptr) {', '}', () {
            indent.writeln(
                'error = g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Failed to encode Flutter message");');
          });
          indent.writeln(
              'g_task_return_error(task, g_steal_pointer(&error));');
          indent.writeln('return;');
        });
        indent.writeln(
            'fl_binary_messenger_send_on_channel(self->messenger, channel_name, message, cancellable, ${methodPrefix}_${methodName}_cb, g_steal_pointer(&task));');''',
    'FlutterApi messenger send',
  );
}

/// Purpose: Decode the response already finished by the transport callback.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose finish method propagates the owned decoded value.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterApiFinish(String source) {
  return _replaceExactlyOnce(
    source,
    r'''        indent.writeln('g_autoptr(GTask) task = G_TASK(result);');
        indent.writeln(
            'GAsyncResult* r = G_ASYNC_RESULT(g_task_propagate_pointer(task, nullptr));');
        indent.writeln(
            'FlBasicMessageChannel* channel = FL_BASIC_MESSAGE_CHANNEL(g_task_get_task_data(task));');
        indent.writeln(
            'g_autoptr(FlValue) response = fl_basic_message_channel_send_finish(channel, r, error);');
        indent.writeScoped('if (response == nullptr) { ', '}', () {
          indent.writeln('return nullptr;');
        });''',
    r'''        indent.writeln('GTask* task = G_TASK(result);');
        indent.writeln(
            'g_autoptr(FlValue) response = static_cast<FlValue*>(g_task_propagate_pointer(task, error));');
        indent.writeScoped('if (response == nullptr) {', '}', () {
          indent.writeln('return nullptr;');
        });''',
    'FlutterApi response finish',
  );
}

/// Purpose: Repair generated FlutterApi transport ownership without relying on
/// Flutter engine channel-wrapper ownership conventions.
///
/// @param source is the pinned GObject generator source.
/// @returns Source with a messenger-level generated transport implementation.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterApiTransport(String source) {
  var patched = _patchFlutterApiCallback(source);
  patched = _patchFlutterApiSend(patched);
  return _patchFlutterApiFinish(patched);
}

/// Purpose: Document every generated GObject structure at its shared template.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose generated structures carry lifecycle documentation.
/// @throws StateError when the pinned generator source has drifted.
String _patchStructureDocumentation(String source) {
  return _replaceExactlyOnce(
    source,
    r'''  indent.writeScoped('struct _$className {', '};', () {''',
    r'''  indent.writeln(
      '/** Purpose: Store generated $name state across its GObject lifecycle. @param parent_instance anchors the state in $parentClassName. @returns Instances retain generated channel data until disposal. @throws Nothing. */');
  indent.writeScoped('struct _$className {', '};', () {''',
    'generated GObject structure documentation',
  );
}

/// Purpose: Document generated GObject lifecycle definitions.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose lifecycle templates emit API documentation.
/// @throws StateError when the pinned generator source has drifted.
String _patchLifecycleDocumentation(String source) {
  var patched = _replaceExactlyOnce(
    source,
    r'''  indent.writeScoped(
      'static void ${methodPrefix}_dispose(GObject* object) {', '}', () {''',
    r'''  indent.writeln(
      '/** Purpose: Release generated $name resources. @param object owns the instance. @returns Nothing. @throws Nothing. */');
  indent.writeScoped(
      'static void ${methodPrefix}_dispose(GObject* object) {', '}', () {''',
    'generated GObject disposal documentation',
  );
  patched = _replaceExactlyOnce(
    patched,
    r'''  indent.writeScoped(
      'static void ${methodPrefix}_init($className* self) {', '}', () {''',
    r'''  indent.writeln(
      '/** Purpose: Initialize generated $name state. @param self is the new instance. @returns Nothing. @throws Nothing. */');
  indent.writeScoped(
      'static void ${methodPrefix}_init($className* self) {', '}', () {''',
    'generated GObject initialization documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''  indent.writeScoped(
      'static void ${methodPrefix}_class_init(${className}Class* klass) {', '}',''',
    r'''  indent.writeln(
      '/** Purpose: Bind generated $name lifecycle methods. @param klass is the generated class. @returns Nothing. @throws Nothing. */');
  indent.writeScoped(
      'static void ${methodPrefix}_class_init(${className}Class* klass) {', '}',''',
    'generated GObject class documentation',
  );
}

/// Purpose: Document generated codec definitions.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose codec templates emit API documentation.
/// @throws StateError when the pinned generator source has drifted.
String _patchCodecDocumentation(String source) {
  var patched = _replaceExactlyOnce(
    source,
    r'''    indent.writeScoped(
        'static gboolean ${codecMethodPrefix}_write_value($_standardCodecName* codec, GByteArray* buffer, FlValue* value, GError** error) {',''',
    r'''    indent.writeln(
        '/** Purpose: Encode one generated channel value. @param codec is the active codec. @param buffer receives bytes. @param value is encoded. @param error receives failure. @returns TRUE on success. @throws Nothing. */');
    indent.writeScoped(
        'static gboolean ${codecMethodPrefix}_write_value($_standardCodecName* codec, GByteArray* buffer, FlValue* value, GError** error) {',''',
    'generated codec writer documentation',
  );
  patched = _replaceExactlyOnce(
    patched,
    r'''    indent.writeScoped(
        'static FlValue* ${codecMethodPrefix}_read_value_of_type($_standardCodecName* codec, GBytes* buffer, size_t* offset, int type, GError** error) {',''',
    r'''    indent.writeln(
        '/** Purpose: Decode one generated channel value. @param codec is the active codec. @param buffer supplies bytes. @param offset advances after decoding. @param type selects the value codec. @param error receives failure. @returns A newly owned value or null. @throws Nothing. */');
    indent.writeScoped(
        'static FlValue* ${codecMethodPrefix}_read_value_of_type($_standardCodecName* codec, GBytes* buffer, size_t* offset, int type, GError** error) {',''',
    'generated codec reader documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''    indent.writeScoped(
        'static $codecClassName* ${codecMethodPrefix}_new() {', '}', () {''',
    r'''    indent.writeln(
        '/** Purpose: Create the generated channel codec. @param None. @returns A newly owned codec. @throws Nothing. */');
    indent.writeScoped(
        'static $codecClassName* ${codecMethodPrefix}_new() {', '}', () {''',
    'generated codec constructor documentation',
  );
}

/// Purpose: Document generated HostApi success and error response constructors.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose host response constructors state ownership contracts.
/// @throws StateError when the pinned generator source has drifted.
String _patchHostResponseDocumentation(String source) {
  final patched = _replaceExactlyOnce(
    source,
    r'''      indent.writeScoped(
          "${method.isAsynchronous ? 'static ' : ''}$responseClassName* ${responseMethodPrefix}_new(${constructorArgs.join(', ')}) {",''',
    r'''      indent.writeln('/**');
      indent.writeln(
          ' * Purpose: Build a successful generated $responseName host response.');
      if (constructorArgs.isEmpty) {
        indent.writeln(' * @param None.');
      } else {
        indent.writeln(
            ' * @param return_value is encoded into the successful response.');
        if (_isNumericListType(method.returnType)) {
          indent.writeln(
              ' * @param return_value_length bounds the numeric list.');
        }
      }
      indent.writeln(' * @returns A newly owned $responseClassName.');
      indent.writeln(' * @throws Nothing.');
      indent.writeln(' */');
      indent.writeScoped(
          "${method.isAsynchronous ? 'static ' : ''}$responseClassName* ${responseMethodPrefix}_new(${constructorArgs.join(', ')}) {",''',
    'generated HostApi success constructor documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''      indent.writeScoped(
          '${method.isAsynchronous ? 'static ' : ''}$responseClassName* ${responseMethodPrefix}_new_error(const gchar* code, const gchar* message, FlValue* details) {',''',
    r'''      indent.writeln(
          '/** Purpose: Build a generated $responseName host error. @param code identifies the stable error. @param message explains the failure. @param details carries optional structured context. @returns A newly owned $responseClassName. @throws Nothing. */');
      indent.writeScoped(
          '${method.isAsynchronous ? 'static ' : ''}$responseClassName* ${responseMethodPrefix}_new_error(const gchar* code, const gchar* message, FlValue* details) {',''',
    'generated HostApi error constructor documentation',
  );
}

/// Purpose: Document generated host-channel dispatch callbacks.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose HostApi callback template emits API documentation.
/// @throws StateError when the pinned generator source has drifted.
String _patchHostCallbackDocumentation(String source) {
  return _replaceExactlyOnce(
    source,
    r'''      indent.writeScoped(
          'static void ${methodPrefix}_${methodName}_cb(FlBasicMessageChannel* channel, FlValue* message_, FlBasicMessageChannelResponseHandle* response_handle, gpointer user_data) {',''',
    r'''      indent.writeln(
          '/** Purpose: Decode and dispatch one generated host call. @param channel carries the call. @param message_ contains arguments. @param response_handle receives the reply. @param user_data owns API state. @returns Nothing. @throws Nothing. */');
      indent.writeScoped(
          'static void ${methodPrefix}_${methodName}_cb(FlBasicMessageChannel* channel, FlValue* message_, FlBasicMessageChannelResponseHandle* response_handle, gpointer user_data) {',''',
    'generated host callback documentation',
  );
}

/// Purpose: Document generated HostApi state and channel registration helpers.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose HostApi setup functions declare ownership and errors.
/// @throws StateError when the pinned generator source has drifted.
String _patchHostSetupDocumentation(String source) {
  var patched = _replaceExactlyOnce(
    source,
    r'''    indent.writeScoped(
        'static $className* ${methodPrefix}_new(const $vtableName* vtable, gpointer user_data, GDestroyNotify user_data_free_func) {',''',
    r'''    indent.writeln(
        '/** Purpose: Bind generated HostApi callbacks to their owned context. @param vtable dispatches host methods. @param user_data is passed to callbacks. @param user_data_free_func releases callback context. @returns A newly owned $className. @throws Nothing. */');
    indent.writeScoped(
        'static $className* ${methodPrefix}_new(const $vtableName* vtable, gpointer user_data, GDestroyNotify user_data_free_func) {',''',
    'generated HostApi state constructor documentation',
  );
  patched = _replaceExactlyOnce(
    patched,
    r'''    indent.writeScoped(
        'void ${methodPrefix}_set_method_handlers(FlBinaryMessenger* messenger, const gchar* suffix, const $vtableName* vtable, gpointer user_data, GDestroyNotify user_data_free_func) {',''',
    r'''    indent.writeln(
        '/** Purpose: Register every generated HostApi channel handler. @param messenger owns channel registrations. @param suffix scopes channel names. @param vtable dispatches host methods. @param user_data is passed to callbacks. @param user_data_free_func releases callback context. @returns Nothing. @throws Nothing. */');
    indent.writeScoped(
        'void ${methodPrefix}_set_method_handlers(FlBinaryMessenger* messenger, const gchar* suffix, const $vtableName* vtable, gpointer user_data, GDestroyNotify user_data_free_func) {',''',
    'generated HostApi handler registration documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''    indent.writeScoped(
        'void ${methodPrefix}_clear_method_handlers(FlBinaryMessenger* messenger, const gchar* suffix) {',''',
    r'''    indent.writeln(
        '/** Purpose: Remove every generated HostApi channel handler. @param messenger owns channel registrations. @param suffix scopes channel names. @returns Nothing. @throws Nothing. */');
    indent.writeScoped(
        'void ${methodPrefix}_clear_method_handlers(FlBinaryMessenger* messenger, const gchar* suffix) {',''',
    'generated HostApi handler cleanup documentation',
  );
}

/// Purpose: Document generated FlutterApi and response object constructors.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose FlutterApi constructors state ownership contracts.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterObjectDocumentation(String source) {
  final patched = _replaceExactlyOnce(
    source,
    r'''    indent.writeScoped(
        '$className* ${methodPrefix}_new(FlBinaryMessenger* messenger, const gchar* suffix) {',''',
    r'''    indent.writeln(
        '/** Purpose: Create a generated FlutterApi transport client. @param messenger carries generated calls. @param suffix scopes channel names. @returns A newly owned $className. @throws Nothing. */');
    indent.writeScoped(
        '$className* ${methodPrefix}_new(FlBinaryMessenger* messenger, const gchar* suffix) {',''',
    'generated FlutterApi constructor documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''      indent.writeScoped(
          'static $responseClassName* ${responseMethodPrefix}_new(FlValue* response) {',''',
    r'''      indent.writeln(
          '/** Purpose: Decode one generated FlutterApi response envelope. @param response contains the returned value or error. @returns A newly owned $responseClassName. @throws Nothing. */');
      indent.writeScoped(
          'static $responseClassName* ${responseMethodPrefix}_new(FlValue* response) {',''',
    'generated FlutterApi response constructor documentation',
  );
}

/// Purpose: Document generated FlutterApi response inspection functions.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose response accessors declare values and preconditions.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterAccessorDocumentation(String source) {
  var patched = _replaceExactlyOnce(
    source,
    r'''      indent.writeScoped(
          'gboolean ${responseMethodPrefix}_is_error($responseClassName* self) {',''',
    r'''      indent.writeln(
          '/** Purpose: Report whether a generated FlutterApi response is an error. @param self is the decoded response. @returns TRUE for an error envelope. @throws Nothing. */');
      indent.writeScoped(
          'gboolean ${responseMethodPrefix}_is_error($responseClassName* self) {',''',
    'generated FlutterApi error predicate documentation',
  );
  patched = _replaceExactlyOnce(
    patched,
    r'''      indent.writeScoped(
          'const gchar* ${responseMethodPrefix}_get_error_code($responseClassName* self) {',''',
    r'''      indent.writeln(
          '/** Purpose: Read the stable code from a generated FlutterApi error. @param self is an error response. @returns A borrowed error code. @throws Nothing; self must contain an error. */');
      indent.writeScoped(
          'const gchar* ${responseMethodPrefix}_get_error_code($responseClassName* self) {',''',
    'generated FlutterApi error code documentation',
  );
  patched = _replaceExactlyOnce(
    patched,
    r'''      indent.writeScoped(
          'const gchar* ${responseMethodPrefix}_get_error_message($responseClassName* self) {',''',
    r'''      indent.writeln(
          '/** Purpose: Read the message from a generated FlutterApi error. @param self is an error response. @returns A borrowed error message. @throws Nothing; self must contain an error. */');
      indent.writeScoped(
          'const gchar* ${responseMethodPrefix}_get_error_message($responseClassName* self) {',''',
    'generated FlutterApi error message documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''      indent.writeScoped(
          'FlValue* ${responseMethodPrefix}_get_error_details($responseClassName* self) {',''',
    r'''      indent.writeln(
          '/** Purpose: Read structured details from a generated FlutterApi error. @param self is an error response. @returns Borrowed error details. @throws Nothing; self must contain an error. */');
      indent.writeScoped(
          'FlValue* ${responseMethodPrefix}_get_error_details($responseClassName* self) {',''',
    'generated FlutterApi error details documentation',
  );
}

/// Purpose: Document optional generated FlutterApi success-value accessors.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose return-value accessors document every output argument.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterReturnDocumentation(String source) {
  return _replaceExactlyOnce(
    source,
    r'''        indent.writeScoped(
            '$returnType ${responseMethodPrefix}_get_return_value($responseClassName* self${_isNumericListType(method.returnType) ? ', size_t* return_value_length' : ''}) {',''',
    r'''        indent.writeln('/**');
        indent.writeln(
            ' * Purpose: Read the successful value from a generated FlutterApi response.');
        indent.writeln(' * @param self is a successful response.');
        if (_isNumericListType(method.returnType)) {
          indent.writeln(
              ' * @param return_value_length receives the numeric list length.');
        }
        indent.writeln(' * @returns The borrowed or primitive return value.');
        indent.writeln(
            ' * @throws Nothing; self must contain a successful response.');
        indent.writeln(' */');
        indent.writeScoped(
            '$returnType ${responseMethodPrefix}_get_return_value($responseClassName* self${_isNumericListType(method.returnType) ? ', size_t* return_value_length' : ''}) {',''',
    'generated FlutterApi return value documentation',
  );
}

/// Purpose: Document generated FlutterApi asynchronous send and finish entrypoints.
///
/// @param source is the pinned GObject generator source.
/// @returns Source whose asynchronous methods document all callback contracts.
/// @throws StateError when the pinned generator source has drifted.
String _patchFlutterMethodDocumentation(String source) {
  final patched = _replaceExactlyOnce(
    source,
    r'''      indent.newln();
      indent.writeScoped(
          "void ${methodPrefix}_$methodName(${asyncArgs.join(', ')}) {", '}',
          () {''',
    r'''      indent.newln();
      indent.writeln('/**');
      indent.writeln(
          ' * Purpose: Send one generated ${api.name}.${method.name} call to Flutter.');
      indent.writeln(' * @param self owns the FlutterApi transport.');
      for (final Parameter param in method.parameters) {
        final String name = _snakeCaseFromCamelCase(param.name);
        indent.writeln(' * @param $name carries ${param.name}.');
        if (_isNumericListType(param.type)) {
          indent.writeln(' * @param ${name}_length bounds $name.');
        }
      }
      indent.writeln(' * @param cancellable controls the transport operation.');
      indent.writeln(' * @param callback receives the asynchronous result.');
      indent.writeln(' * @param user_data is passed to callback.');
      indent.writeln(' * @returns Nothing.');
      indent.writeln(
          ' * @throws Nothing; transport errors complete callback.');
      indent.writeln(' */');
      indent.writeScoped(
          "void ${methodPrefix}_$methodName(${asyncArgs.join(', ')}) {", '}',
          () {''',
    'generated FlutterApi send documentation',
  );
  return _replaceExactlyOnce(
    patched,
    r'''      indent.newln();
      indent.writeScoped(
          "$responseClassName* ${methodPrefix}_${methodName}_finish(${finishArgs.join(', ')}) {",
          '}', () {''',
    r'''      indent.newln();
      indent.writeln(
          '/** Purpose: Finish one generated ${api.name}.${method.name} call. @param self owns the FlutterApi transport. @param result is the completed forwarding task. @param error receives transport or codec failure. @returns A newly owned response or null. @throws Nothing. */');
      indent.writeScoped(
          "$responseClassName* ${methodPrefix}_${methodName}_finish(${finishArgs.join(', ')}) {",
          '}', () {''',
    'generated FlutterApi finish documentation',
  );
}

/// Purpose: Correct the isolated pinned generator before binding emission.
///
/// @param generatorFile is the copied gobject_generator.dart source.
/// @returns A future that completes after every validated patch is written.
/// @throws StateError when the pinned generator source has drifted.
Future<void> _patchGenerator(File generatorFile) async {
  var source = await generatorFile.readAsString();
  source = _patchFlutterApiTransport(source);
  source = _patchStructureDocumentation(source);
  source = _patchLifecycleDocumentation(source);
  source = _patchCodecDocumentation(source);
  source = _patchHostResponseDocumentation(source);
  source = _patchHostCallbackDocumentation(source);
  source = _patchHostSetupDocumentation(source);
  source = _patchFlutterObjectDocumentation(source);
  source = _patchFlutterAccessorDocumentation(source);
  source = _patchFlutterReturnDocumentation(source);
  source = _patchFlutterMethodDocumentation(source);
  await generatorFile.writeAsString(source);
}

/// Purpose: Resolve the pinned Pigeon package and create a package config that
/// redirects only Pigeon to the patched temporary copy.
///
/// @param packageConfig is this package's dependency resolution.
/// @param patchedRoot is the isolated Pigeon package root.
/// @param temporaryRoot owns the rewritten package_config.json.
/// @returns The rewritten package-config file.
/// @throws StateError when Pigeon is absent or not pinned to 22.7.4.
Future<File> _createPatchedPackageConfig(
  File packageConfig,
  Directory patchedRoot,
  Directory temporaryRoot,
) async {
  final config =
      jsonDecode(await packageConfig.readAsString()) as Map<String, Object?>;
  final rawPackages = config['packages'];
  if (rawPackages is! List<Object?>) {
    throw const FormatException('package_config.json has no package list.');
  }
  final packages = <Map<String, Object?>>[];
  for (final rawPackage in rawPackages) {
    if (rawPackage is! Map<String, Object?> ||
        rawPackage['rootUri'] is! String) {
      throw const FormatException('package_config.json entry is malformed.');
    }
    packages.add(rawPackage);
  }
  final pigeon = packages.singleWhere((entry) => entry['name'] == 'pigeon');
  final resolvedRoot = Directory.fromUri(
    packageConfig.uri.resolve(pigeon['rootUri']! as String),
  );
  final pubspec =
      await File('${resolvedRoot.path}/pubspec.yaml').readAsString();
  if (!RegExp(r'^version:\s*22\.7\.4(?:\s|$)', multiLine: true)
      .hasMatch(pubspec)) {
    throw StateError(
      'Expected Pigeon $_pigeonVersion at ${resolvedRoot.path}.',
    );
  }

  await _copyDirectory(resolvedRoot, patchedRoot);
  await _patchGenerator(
    File('${patchedRoot.path}/lib/gobject_generator.dart'),
  );
  for (final package in packages) {
    package['rootUri'] =
        packageConfig.uri.resolve(package['rootUri']! as String).toString();
  }
  pigeon['rootUri'] = patchedRoot.uri.toString();
  final patchedConfig = File('${temporaryRoot.path}/package_config.json');
  await patchedConfig.writeAsString(jsonEncode(config));
  return patchedConfig;
}

/// Purpose: Parse the optional destination while rejecting ambiguous commands.
///
/// @param arguments are command-line options after the script name.
/// @param packageRoot is the default checked-in generation destination.
/// @returns The absolute output directory.
/// @throws FormatException for unsupported or incomplete arguments.
Directory _parseOutputRoot(List<String> arguments, Directory packageRoot) {
  if (arguments.isEmpty) {
    return packageRoot.absolute;
  }
  if (arguments.length == 2 && arguments.first == '--output-root') {
    return Directory(arguments.last).absolute;
  }
  throw const FormatException(
    'Usage: dart run tool/generate_pigeon.dart '
    '[--output-root <directory>]',
  );
}

/// Purpose: Normalize generated Dart so regeneration is byte-stable and does
/// not depend on whether a developer formats after running Pigeon.
///
/// @param outputRoot contains Pigeon's generated Dart output.
/// @returns A future that completes after formatting succeeds.
/// @throws [FormatterException] when the generated source is invalid.
Future<void> _formatDartOutput(Directory outputRoot) async {
  final output = '${outputRoot.path}/lib/src/generated/native_query.g.dart';
  final file = File(output);
  final formatter = DartFormatter(
    languageVersion: DartFormatter.latestShortStyleLanguageVersion,
  );
  await file.writeAsString(
    formatter.format(await file.readAsString(), uri: file.uri),
  );
}

/// Purpose: Regenerate Dart and Linux GObject bindings reproducibly using the
/// pinned Pigeon package plus the ownership repair above.
///
/// @param arguments optionally select a temporary output root for tests.
/// @returns A future that completes when generation succeeds.
/// @throws ProcessException when Pigeon exits unsuccessfully.
Future<void> main(List<String> arguments) async {
  final script = File.fromUri(Platform.script).absolute;
  final packageRoot = script.parent.parent;
  final outputRoot = _parseOutputRoot(arguments, packageRoot);
  final packageConfig = File(
    '${packageRoot.path}/.dart_tool/package_config.json',
  );
  if (!await packageConfig.exists()) {
    throw StateError(
      'Missing ${packageConfig.path}; run flutter pub get first.',
    );
  }
  await outputRoot.create(recursive: true);

  final temporaryRoot =
      await Directory.systemTemp.createTemp('simple_query_pigeon_');
  try {
    final patchedRoot = Directory(
      '${temporaryRoot.path}/pigeon-$_pigeonVersion',
    );
    final patchedConfig = await _createPatchedPackageConfig(
      packageConfig,
      patchedRoot,
      temporaryRoot,
    );
    final result = await Process.run(
      Platform.resolvedExecutable,
      <String>[
        '--disable-dart-dev',
        '--packages=${patchedConfig.path}',
        '${patchedRoot.path}/bin/pigeon.dart',
        '--input',
        '${packageRoot.path}/pigeon.dart',
      ],
      workingDirectory: outputRoot.path,
      environment: <String, String>{
        ...Platform.environment,
        'DASH__SUPPRESS_ANALYTICS': 'true',
      },
    );
    stdout.write(result.stdout);
    stderr.write(result.stderr);
    if (result.exitCode != 0) {
      throw ProcessException(
        Platform.resolvedExecutable,
        const <String>['<patched Pigeon command>'],
        'Pigeon generation failed.',
        result.exitCode,
      );
    }
    await _formatDartOutput(outputRoot);
  } finally {
    await temporaryRoot.delete(recursive: true);
  }
}
