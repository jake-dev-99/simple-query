import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

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
