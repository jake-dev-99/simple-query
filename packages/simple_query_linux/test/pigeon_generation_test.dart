import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Purpose: Capture generated-output existence and bytes before regeneration.
///
/// @param files lists every checkout artifact guarded by the test.
/// @returns A future snapshot keyed by absolute path.
/// @throws [FileSystemException] when an existing artifact cannot be read.
Future<Map<String, List<int>?>> _snapshotFiles(List<File> files) async {
  final snapshot = <String, List<int>?>{};
  for (final file in files) {
    snapshot[file.path] = await file.exists() ? await file.readAsBytes() : null;
  }
  return snapshot;
}

/// Purpose: Prove generation never mutates guarded checkout artifacts.
///
/// @param snapshot contains the pre-generation existence and bytes.
/// @returns A future that completes after every invariant is checked.
/// @throws [FileSystemException] when an existing artifact cannot be read.
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

/// Purpose: Verify every native target compiles the real decomposed sources.
///
/// @param packageDirectory is the Linux package root.
/// @returns A future that completes after CMake and test-runner assertions.
/// @throws [FileSystemException] when source wiring cannot be read.
Future<void> _expectNativeSourceWiring(Directory packageDirectory) async {
  final cmake = await File('${packageDirectory.path}/linux/CMakeLists.txt')
      .readAsString();
  final nativeRunner = await File(
    '${packageDirectory.path}/tool/run_native_tests.sh',
  ).readAsString();
  final nativeHarness = await File(
    '${packageDirectory.path}/linux/test/simple_query_linux_plugin_test.cc',
  ).readAsString();
  for (final filename in <String>[
    'simple_query_linux_plugin.cc',
    'simple_query_linux_binary.cc',
    'simple_query_linux_helpers.cc',
    'simple_query_linux_query.cc',
    'simple_query_linux_mutation.cc',
    'simple_query_linux_observer.cc',
    'native_query.g.cc',
  ]) {
    expect(cmake, contains('"$filename"'));
    expect(nativeRunner, contains('/linux/$filename"'));
  }
  expect(nativeHarness, isNot(contains('#include "../native_query.g.cc"')));
  expect(
    nativeHarness,
    isNot(contains('#include "../simple_query_linux_plugin.cc"')),
  );
}

/// Purpose: Register deterministic Linux Pigeon generation verification.
///
/// @returns Nothing.
/// @throws Nothing directly; the registered test reports failures.
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

    final result = await Process.run(
      'dart',
      <String>[
        'run',
        File(
          '${packageDirectory.path}/tool/generate_pigeon.dart',
        ).absolute.path,
        '--output-root',
        outputDirectory.absolute.path,
      ],
      workingDirectory: packageDirectory.path,
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
    final linuxSources = '$header\n$source';

    expect(header, contains('#include <flutter_linux/flutter_linux.h>'));
    expect(
      header,
      isNot(contains('#include <flutter/basic_message_channel.h>')),
    );
    expect(header, isNot(contains('const gchar* namespace,')));
    expect(linuxSources, isNot(contains('flutter::')));
    expect(linuxSources, isNot(contains('#include <flutter/')));
    await _expectNativeSourceWiring(packageDirectory);
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
    for (final relativePath in <String>[
      'lib/src/generated/native_query.g.dart',
      'linux/native_query.g.h',
      'linux/native_query.g.cc',
    ]) {
      expect(
        await File('${outputDirectory.path}/$relativePath').readAsBytes(),
        await File('${packageDirectory.path}/$relativePath').readAsBytes(),
        reason: '$relativePath is not reproducible from pigeon.dart.',
      );
    }
  });
}
