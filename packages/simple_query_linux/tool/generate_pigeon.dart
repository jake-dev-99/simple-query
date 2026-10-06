import 'dart:convert';
import 'dart:io';

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

/// Purpose: Correct Pigeon's GTask and GAsyncResult ownership in an isolated
/// copy of the pinned generator before it emits checked-in Linux bindings.
///
/// @param generatorFile is the copied gobject_generator.dart source.
/// @returns A future that completes after the validated patch is written.
/// @throws StateError when the pinned generator source has drifted.
Future<void> _patchGenerator(File generatorFile) async {
  var source = await generatorFile.readAsString();
  source = _replaceExactlyOnce(
    source,
    """        indent.writeln('GTask* task = G_TASK(user_data);');
        indent.writeln('g_task_return_pointer(task, result, g_object_unref);');""",
    """        indent.writeln('g_autoptr(GTask) task = G_TASK(user_data);');
        indent.writeln(
            'g_task_return_pointer(task, g_object_ref(result), g_object_unref);');""",
    'async callback ownership',
  );
  source = _replaceExactlyOnce(
    source,
    """        indent.writeln('g_autoptr(GTask) task = G_TASK(result);');
        indent.writeln(
            'GAsyncResult* r = G_ASYNC_RESULT(g_task_propagate_pointer(task, nullptr));');""",
    """        indent.writeln('GTask* task = G_TASK(result);');
        indent.writeln(
            'g_autoptr(GAsyncResult) r = G_ASYNC_RESULT(g_task_propagate_pointer(task, error));');
        indent.writeScoped('if (r == nullptr) {', '}', () {
          indent.writeln('return nullptr;');
        });""",
    'async finish ownership',
  );
  source = _replaceExactlyOnce(
    source,
    "indent.writeScoped('if (response == nullptr) { ', '}', () {",
    "indent.writeScoped('if (response == nullptr) {', '}', () {",
    'generated trailing whitespace',
  );
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
/// @throws ProcessException when the Dart formatter exits unsuccessfully.
Future<void> _formatDartOutput(Directory outputRoot) async {
  final output = '${outputRoot.path}/lib/src/generated/native_query.g.dart';
  final result = await Process.run(
    Platform.resolvedExecutable,
    <String>[
      '--suppress-analytics',
      'format',
      '--language-version=3.6',
      output,
    ],
    environment: <String, String>{
      ...Platform.environment,
      'DASH__SUPPRESS_ANALYTICS': 'true',
    },
  );
  if (result.exitCode != 0) {
    throw ProcessException(
      Platform.resolvedExecutable,
      <String>['format', '--language-version=3.6', output],
      '${result.stdout}\n${result.stderr}',
      result.exitCode,
    );
  }
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
