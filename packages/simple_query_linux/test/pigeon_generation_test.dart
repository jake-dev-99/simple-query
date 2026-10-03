import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

String _functionBody(String source, String signature) {
  final signatureIndex = source.indexOf(signature);
  if (signatureIndex == -1) {
    throw StateError('Could not find $signature.');
  }
  final openingBrace = source.indexOf('{', signatureIndex);
  if (openingBrace == -1) {
    throw StateError('Could not find the body of $signature.');
  }
  var depth = 0;
  for (var index = openingBrace; index < source.length; index++) {
    if (source[index] == '{') {
      depth++;
    } else if (source[index] == '}') {
      depth--;
      if (depth == 0) {
        return source.substring(openingBrace + 1, index);
      }
    }
  }
  throw StateError('Could not find the end of $signature.');
}

Future<Map<String, List<int>?>> _snapshotFiles(List<File> files) async {
  final snapshot = <String, List<int>?>{};
  for (final file in files) {
    snapshot[file.path] = await file.exists() ? await file.readAsBytes() : null;
  }
  return snapshot;
}

Future<void> _expectFilesUnchanged(
  Map<String, List<int>?> snapshot,
) async {
  for (final entry in snapshot.entries) {
    final file = File(entry.key);
    final exists = await file.exists();
    expect(
      exists,
      entry.value != null,
      reason: '${file.path} changed existence during Pigeon generation.',
    );
    if (exists && entry.value != null) {
      expect(
        await file.readAsBytes(),
        entry.value,
        reason: '${file.path} changed during Pigeon generation.',
      );
    }
  }
}

void main() {
  test('Pigeon generation emits Linux GObject bindings', () async {
    final packageDirectory = Directory.current.absolute;
    final outputDirectory =
        await Directory.systemTemp.createTemp('simple_query_linux_pigeon_');
    final checkedInGeneratedFiles = <File>[
      File('${packageDirectory.path}/lib/src/generated/native_query.g.dart'),
      File('${packageDirectory.path}/linux/native_query.g.h'),
      File('${packageDirectory.path}/linux/native_query.g.cc'),
      File('${packageDirectory.path}/linux/native_query.g.cpp'),
    ];
    final checkedInGeneratedSnapshot =
        await _snapshotFiles(checkedInGeneratedFiles);
    addTearDown(() async {
      try {
        await _expectFilesUnchanged(checkedInGeneratedSnapshot);
      } finally {
        if (await outputDirectory.exists()) {
          await outputDirectory.delete(recursive: true);
        }
      }
    });

    final packageConfig = File(
      '${packageDirectory.path}/.dart_tool/package_config.json',
    );
    final config =
        jsonDecode(await packageConfig.readAsString()) as Map<String, Object?>;
    final packages =
        (config['packages']! as List<Object?>).cast<Map<String, Object?>>();
    final pigeon =
        packages.singleWhere((package) => package['name'] == 'pigeon');
    final pigeonRoot = Directory.fromUri(
      packageConfig.uri.resolve(pigeon['rootUri']! as String),
    );
    final pigeonExecutable = File('${pigeonRoot.path}/bin/pigeon.dart');
    expect(
      await pigeonExecutable.exists(),
      isTrue,
      reason: 'Pigeon is missing from the resolved package cache.',
    );

    final result = await Process.run(
      'dart',
      <String>[
        '--packages=${packageConfig.absolute.path}',
        pigeonExecutable.absolute.path,
        '--input',
        File('${packageDirectory.path}/pigeon.dart').absolute.path,
      ],
      workingDirectory: outputDirectory.path,
    );

    expect(
      result.exitCode,
      0,
      reason: 'Pigeon failed:\n${result.stdout}\n${result.stderr}',
    );

    final header = await File(
      '${outputDirectory.path}/linux/native_query.g.h',
    ).readAsString();
    final source = await File(
      '${outputDirectory.path}/linux/native_query.g.cc',
    ).readAsString();
    final plugin = await File(
      '${packageDirectory.path}/linux/simple_query_linux_plugin.cc',
    ).readAsString();
    final linuxSources = '$header\n$source\n$plugin';
    final registrationBody = _functionBody(
      plugin,
      'void simple_query_linux_plugin_register_with_registrar(',
    );
    final disposeBody = _functionBody(
      plugin,
      'static void simple_query_linux_plugin_dispose(',
    );

    expect(header, contains('#include <flutter_linux/flutter_linux.h>'));
    expect(
      header,
      isNot(contains('#include <flutter/basic_message_channel.h>')),
    );
    expect(header, isNot(contains('const gchar* namespace,')));
    expect(linuxSources, isNot(contains('flutter::')));
    expect(linuxSources, isNot(contains('#include <flutter/')));
    expect(
      registrationBody,
      contains('sqlq_native_query_host_api_set_method_handlers('),
    );
    expect(
      disposeBody,
      contains('sqlq_native_query_host_api_clear_method_handlers('),
    );
    expect(
      File('${outputDirectory.path}/linux/native_query.g.cc').existsSync(),
      isTrue,
    );
    expect(
      File('${outputDirectory.path}/linux/native_query.g.cpp').existsSync(),
      isFalse,
    );
    expect(
      File(
        '${outputDirectory.path}/lib/src/generated/native_query.g.dart',
      ).existsSync(),
      isTrue,
    );
  });
}
