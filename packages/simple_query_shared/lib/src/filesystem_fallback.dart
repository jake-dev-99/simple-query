import 'dart:async';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart';

typedef SimpleQueryErrorBuilder = SimpleQueryError Function({
  required SimpleQueryErrorCode code,
  required String message,
  QueryDomain? domain,
  QueryOperation? operation,
  Map<String, Object?>? details,
});

class LocalFileSystemFallback {
  LocalFileSystemFallback({
    required this.source,
    required this.unsupportedReasonFor,
    this.defaultRootPath,
    SimpleQueryErrorBuilder? errorBuilder,
  }) : _errorBuilder = errorBuilder ?? _defaultErrorBuilder;

  final String source;
  final String Function(QueryDomain domain) unsupportedReasonFor;

  /// Platform-specific default root path for files/media queries when the
  /// caller does not provide `platformData.rootPath`. When null, rootPath is
  /// required from the caller.
  final String? defaultRootPath;

  final SimpleQueryErrorBuilder _errorBuilder;

  final Map<String, String> _openHandles = <String, String>{};
  final Set<Timer> _observeTimers = <Timer>{};

  static SimpleQueryError _defaultErrorBuilder({
    required SimpleQueryErrorCode code,
    required String message,
    QueryDomain? domain,
    QueryOperation? operation,
    Map<String, Object?>? details,
  }) {
    return SimpleQueryError(
      code: code,
      message: message,
      domain: domain,
      operation: operation,
      details: details,
    );
  }

  bool supportsDomain(QueryDomain domain) {
    return domain == QueryDomain.files || domain == QueryDomain.media;
  }

  String requireRootPath(Map<String, Object?>? platformData) {
    final rootPath = platformData?['rootPath']?.toString();
    if (rootPath != null && rootPath.isNotEmpty) {
      return _normalizeRootPath(rootPath);
    }
    if (defaultRootPath != null && defaultRootPath!.isNotEmpty) {
      return _normalizeRootPath(defaultRootPath!);
    }
    throw _invalidQuery(
      'simple_query: filesystem fallback requires platformData.rootPath',
    );
  }

  Future<QueryResult> query(QueryRequest request) async {
    _ensureSupported(
      domain: request.domain,
      operation: QueryOperation.read,
    );

    return _guard<QueryResult>(
      domain: request.domain,
      operation: QueryOperation.read,
      run: () async {
        final rootPath = requireRootPath(request.platformData);
        final records = await listFileRecords(
          rootPath: rootPath,
          mediaOnly: request.domain == QueryDomain.media,
        );
        final filtered = applyFilters(records, request.filters);
        final sorted = applySort(filtered, request.sort);
        final paged = applyPaging(sorted, request.page);
        final projected = applyProjection(paged.records, request.projection);

        return RuntimeContractValidation.validateQueryResult(
          domain: request.domain,
          result: QueryResult(
            records: projected,
            totalCount: filtered.length,
            nextOffset: paged.nextOffset,
            nextCursor: paged.nextCursor,
            metadata: <String, Object?>{
              'implementation': 'filesystem_fallback',
              'rootPath': rootPath,
              'mediaOnly': request.domain == QueryDomain.media,
            },
          ),
        );
      },
    );
  }

  Future<MutationResult> mutate(MutationRequest request) async {
    _ensureSupported(
      domain: request.domain,
      operation: QueryOperation.write,
    );

    return _guard<MutationResult>(
      domain: request.domain,
      operation: QueryOperation.write,
      run: () async {
        final rootPath = requireRootPath(request.platformData);

        switch (request.type) {
          case MutationType.insert:
            final values = request.values;
            if (values == null) {
              throw _invalidQuery('Insert mutation requires values');
            }
            final path = values['path']?.toString();
            if (path == null || path.isEmpty) {
              throw _invalidQuery('Insert mutation requires values.path');
            }
            final resolvedPath = _resolveWithinRoot(
              rootPath: rootPath,
              candidatePath: path,
              argumentName: 'values.path',
            );

            final isDirectory = values['isDirectory'] == true;
            if (isDirectory) {
              final directory = Directory(resolvedPath);
              await directory.create(recursive: true);
              return _mutationResult(
                  affectedCount: 1, insertedId: resolvedPath);
            }

            final file = File(resolvedPath);
            await file.parent.create(recursive: true);
            final bytes = _coerceBytes(values['bytes']);
            if (bytes != null) {
              await file.writeAsBytes(bytes, flush: true);
            } else {
              await file.writeAsString(
                values['content']?.toString() ?? '',
                flush: true,
              );
            }
            return _mutationResult(
              affectedCount: 1,
              insertedId: resolvedPath,
            );
          case MutationType.update:
            final values = request.values;
            if (values == null) {
              throw _invalidQuery('Update mutation requires values');
            }

            final explicitPath =
                values['path']?.toString() ?? values['id']?.toString();
            final targetPaths = <String>{};
            if (explicitPath != null && explicitPath.isNotEmpty) {
              targetPaths.add(
                _resolveWithinRoot(
                  rootPath: rootPath,
                  candidatePath: explicitPath,
                  argumentName: 'values.path',
                ),
              );
            }
            if (targetPaths.isEmpty) {
              final queryResult = await query(
                QueryRequest(
                  domain: request.domain,
                  filters: request.filters,
                  platformData: request.platformData,
                ),
              );
              for (final record in queryResult.records) {
                final path = record['path']?.toString();
                if (path != null && path.isNotEmpty) {
                  targetPaths.add(path);
                }
              }
            }

            var updated = 0;
            for (final path in targetPaths) {
              if (await _applyLocalFsUpdate(
                rootPath: rootPath,
                path: path,
                values: values,
              )) {
                updated += 1;
              }
            }
            return _mutationResult(affectedCount: updated);
          case MutationType.delete:
            final queryResult = await query(
              QueryRequest(
                domain: request.domain,
                filters: request.filters,
                platformData: request.platformData,
              ),
            );

            var deleted = 0;
            for (final record in queryResult.records) {
              final path = record['path']?.toString();
              if (path == null || path.isEmpty) continue;
              final type = record['type'];
              if (type == 'directory') {
                final directory = Directory(path);
                if (await directory.exists()) {
                  await directory.delete(recursive: true);
                  deleted += 1;
                }
              } else {
                final file = File(path);
                if (await file.exists()) {
                  await file.delete();
                  deleted += 1;
                }
              }
            }

            return _mutationResult(affectedCount: deleted);
        }
      },
    );
  }

  Future<BatchResult> batch(BatchRequest request) async {
    final results = <MutationResult>[];
    for (final operation in request.operations) {
      try {
        final effectivePlatformData = <String, Object?>{
          ...?request.platformData,
          ...?operation.platformData,
        };
        results.add(
          await mutate(
            MutationRequest(
              domain: operation.domain,
              type: operation.type,
              entityType: operation.entityType,
              values: operation.values,
              filters: operation.filters,
              platformData:
                  effectivePlatformData.isEmpty ? null : effectivePlatformData,
            ),
          ),
        );
      } on SimpleQueryError catch (error) {
        results.add(
          MutationResult(
            affectedCount: 0,
            metadata: <String, Object?>{
              'batchSemantics': 'sequentialBestEffort',
              'implementation': 'filesystem_fallback',
              'error': <String, Object?>{
                'code': error.code.name,
                'message': error.message,
                if (error.domain != null) 'domain': error.domain!.name,
                if (error.operation != null) 'operation': error.operation!.name,
              },
            },
          ),
        );
      }
    }
    return BatchResult(results: results);
  }

  Stream<ObserveEvent> observe(ObserveRequest request) {
    if (!supportsDomain(request.domain)) {
      return Stream<ObserveEvent>.error(
        _notSupported(
          domain: request.domain,
          operation: QueryOperation.observe,
        ),
      );
    }
    final rootPath = (() {
      try {
        return requireRootPath(request.platformData);
      } on SimpleQueryError catch (error) {
        return error;
      }
    })();
    if (rootPath is SimpleQueryError) {
      return Stream<ObserveEvent>.error(rootPath);
    }

    late final StreamController<ObserveEvent> controller;
    Map<String, int> snapshot = <String, int>{};
    Timer? timer;

    Future<Map<String, int>> buildSnapshot() async {
      final records = await listFileRecords(
        rootPath: rootPath as String,
        mediaOnly: request.domain == QueryDomain.media,
      );
      return <String, int>{
        for (final record in records)
          if (record['id'] != null)
            record['id']!.toString(): snapshotEpochMs(record),
      };
    }

    controller = StreamController<ObserveEvent>.broadcast(
      onListen: () async {
        snapshot = await buildSnapshot();
        timer = Timer.periodic(
          request.pollingInterval ?? const Duration(seconds: 2),
          (_) async {
            try {
              final next = await buildSnapshot();
              if (sameSnapshot(snapshot, next)) return;

              final changedIds = <String>{
                ...snapshot.keys.where((key) => !next.containsKey(key)),
                ...next.keys.where((key) => !snapshot.containsKey(key)),
                ...next.keys.where(
                  (key) =>
                      snapshot.containsKey(key) && snapshot[key] != next[key],
                ),
              };

              snapshot = next;
              controller.add(
                ObserveEvent(
                  domain: request.domain,
                  entityType: request.entityType,
                  changeType: ObserveChangeType.update,
                  timestamp: DateTime.now().toUtc(),
                  source: source,
                  ids: changedIds.toList(growable: false),
                  metadata: const <String, Object?>{
                    'implementation': 'filesystem_fallback',
                  },
                ),
              );
            } on SimpleQueryError catch (error) {
              controller.addError(error);
            }
          },
        );
        _observeTimers.add(timer!);
      },
      onCancel: () {
        timer?.cancel();
        if (timer != null) {
          _observeTimers.remove(timer);
        }
      },
    );

    return controller.stream;
  }

  Future<BinaryContentHandle> openBinary(BinaryRequest request) async {
    _ensureSupported(
      domain: request.domain,
      operation: QueryOperation.stream,
    );

    return _guard<BinaryContentHandle>(
      domain: request.domain,
      operation: QueryOperation.stream,
      run: () async {
        final rootPath = requireRootPath(request.platformData);
        final requestedPath =
            request.platformData?['path']?.toString() ?? request.recordId;
        if (requestedPath == null || requestedPath.isEmpty) {
          throw _invalidQuery(
            'Binary request requires platformData.path or recordId',
          );
        }
        final path = _resolveWithinRoot(
          rootPath: rootPath,
          candidatePath: requestedPath,
          argumentName: request.platformData?['path'] != null
              ? 'platformData.path'
              : 'recordId',
        );

        final file = File(path);
        if (!await file.exists()) {
          throw _errorBuilder(
            code: SimpleQueryErrorCode.unavailable,
            message: 'Binary file does not exist: $path',
            domain: request.domain,
            operation: QueryOperation.stream,
          );
        }

        final handleId = DateTime.now().microsecondsSinceEpoch.toString();
        _openHandles[handleId] = path;
        final stat = await file.stat();

        return BinaryContentHandle(
          handleId: handleId,
          localPath: path,
          size: stat.size,
          metadata: <String, Object?>{
            'domain': request.domain.name,
            'implementation': 'filesystem_fallback',
          },
        );
      },
    );
  }

  Future<void> closeBinary(String handleId) async {
    _openHandles.remove(handleId);
  }

  Future<List<QueryRecord>> listFileRecords({
    required String rootPath,
    required bool mediaOnly,
  }) async {
    final root = Directory(_normalizeRootPath(rootPath));
    if (!await root.exists()) {
      return const <QueryRecord>[];
    }

    final records = <QueryRecord>[];

    await for (final entity
        in root.list(recursive: true, followLinks: false)) {
      final stat = await entity.stat();
      final type =
          stat.type == FileSystemEntityType.directory ? 'directory' : 'file';
      final path = entity.path;
      final name = path.split(Platform.pathSeparator).last;
      final ext = fileExtension(name);
      final mime = mimeFromExtension(ext);
      final isDirectory = type == 'directory';
      final mediaType = mediaTypeFromMime(mime);

      if (mediaOnly && !isMediaMime(mime)) {
        continue;
      }

      if (mediaOnly) {
        records.add(<String, Object?>{
          'id': path,
          'uriOrPath': path,
          'mediaType': mediaType,
          'mimeType': mime,
          'size': stat.size,
          'createdAt': stat.changed.toUtc().toIso8601String(),
          'modifiedAt': stat.modified.toUtc().toIso8601String(),
        });
      } else {
        records.add(<String, Object?>{
          'id': path,
          'path': path,
          'name': name,
          'isDirectory': isDirectory,
          'extension': ext,
          'mimeType': mime,
          'type': type,
          'size': stat.size,
          'modifiedAt': stat.modified.toUtc().toIso8601String(),
          'modifiedEpochMs': stat.modified.toUtc().millisecondsSinceEpoch,
        });
      }
    }

    return records;
  }

  void dispose() {
    for (final timer in _observeTimers) {
      timer.cancel();
    }
    _observeTimers.clear();
    _openHandles.clear();
  }

  List<QueryRecord> applyFilters(
    List<QueryRecord> records,
    List<QueryFilterCondition> filters,
  ) {
    if (filters.isEmpty) return records;

    return records.where((record) {
      for (final filter in filters) {
        final value = record[filter.field];
        final target = filter.value;
        if (!matchesFilter(value, filter.operator, target)) {
          return false;
        }
      }
      return true;
    }).toList(growable: false);
  }

  List<QueryRecord> applySort(List<QueryRecord> records, List<QuerySort> sort) {
    if (sort.isEmpty) return records;
    final sorted = List<QueryRecord>.from(records);
    sorted.sort((left, right) {
      for (final rule in sort) {
        final result = compare(left[rule.field], right[rule.field]);
        if (result != 0) {
          return rule.direction == QuerySortDirection.descending
              ? -result
              : result;
        }
      }
      return 0;
    });
    return sorted;
  }

  PagedResult applyPaging(List<QueryRecord> records, QueryPage? page) {
    if (page == null || page.limit == null) {
      return PagedResult(records: records, nextOffset: null);
    }

    // Cursor-based: skip records up to and including the cursor id.
    int startOffset;
    if (page.cursor != null && page.cursor!.isNotEmpty) {
      final cursorId = page.cursor!;
      final cursorIndex =
          records.indexWhere((r) => r['id']?.toString() == cursorId);
      startOffset = cursorIndex < 0 ? records.length : cursorIndex + 1;
    } else {
      startOffset = page.offset ?? 0;
    }

    if (startOffset >= records.length) {
      return const PagedResult(records: <QueryRecord>[], nextOffset: null);
    }

    final end = (startOffset + page.limit!).clamp(0, records.length);
    final chunk = records.sublist(startOffset, end);
    final hasMore = end < records.length;
    final nextOffset = hasMore ? end : null;
    final nextCursor =
        hasMore && chunk.isNotEmpty ? chunk.last['id']?.toString() : null;
    return PagedResult(
        records: chunk, nextOffset: nextOffset, nextCursor: nextCursor);
  }

  List<QueryRecord> applyProjection(
    List<QueryRecord> records,
    List<String>? projection,
  ) {
    if (projection == null || projection.isEmpty) return records;
    return records
        .map(
          (record) => <String, Object?>{
            for (final key in projection)
              if (record.containsKey(key)) key: record[key],
          },
        )
        .toList(growable: false);
  }

  bool matchesFilter(
    Object? source,
    QueryFilterOperator operator,
    Object? target,
  ) {
    switch (operator) {
      case QueryFilterOperator.equals:
        return source == target;
      case QueryFilterOperator.notEquals:
        return source != target;
      case QueryFilterOperator.greaterThan:
        return compare(source, target) > 0;
      case QueryFilterOperator.greaterThanOrEqual:
        return compare(source, target) >= 0;
      case QueryFilterOperator.lessThan:
        return compare(source, target) < 0;
      case QueryFilterOperator.lessThanOrEqual:
        return compare(source, target) <= 0;
      case QueryFilterOperator.contains:
        return (source?.toString() ?? '')
            .toLowerCase()
            .contains((target?.toString() ?? '').toLowerCase());
      case QueryFilterOperator.inList:
        if (target is! List<Object?>) return false;
        return target.contains(source);
    }
  }

  int compare(Object? a, Object? b) {
    if (a is num && b is num) {
      return a.compareTo(b);
    }
    return (a?.toString() ?? '').compareTo(b?.toString() ?? '');
  }

  bool sameSnapshot(Map<String, int> a, Map<String, int> b) {
    if (a.length != b.length) return false;
    for (final entry in a.entries) {
      if (b[entry.key] != entry.value) return false;
    }
    return true;
  }

  int snapshotEpochMs(QueryRecord record) {
    final modifiedEpoch = record['modifiedEpochMs'];
    if (modifiedEpoch is int) return modifiedEpoch;
    if (modifiedEpoch is num) return modifiedEpoch.toInt();

    for (final key in <String>['modifiedAt', 'updatedAt', 'startAt']) {
      final value = record[key];
      if (value is int) return value;
      if (value is num) return value.toInt();
      if (value is String) {
        final parsedInt = int.tryParse(value);
        if (parsedInt != null) return parsedInt;
        final parsedDate = DateTime.tryParse(value);
        if (parsedDate != null) {
          return parsedDate.toUtc().millisecondsSinceEpoch;
        }
      }
    }
    return 0;
  }

  String fileExtension(String name) {
    final index = name.lastIndexOf('.');
    if (index < 0 || index == name.length - 1) return '';
    return name.substring(index + 1).toLowerCase();
  }

  String mimeFromExtension(String extension) {
    switch (extension) {
      case 'jpg':
      case 'jpeg':
        return 'image/jpeg';
      case 'png':
        return 'image/png';
      case 'gif':
        return 'image/gif';
      case 'webp':
        return 'image/webp';
      case 'mp4':
      case 'm4v':
        return 'video/mp4';
      case 'mov':
        return 'video/quicktime';
      case 'mp3':
        return 'audio/mpeg';
      case 'wav':
        return 'audio/wav';
      case 'aac':
        return 'audio/aac';
      case 'flac':
        return 'audio/flac';
      case 'txt':
        return 'text/plain';
      case 'json':
        return 'application/json';
      default:
        return 'application/octet-stream';
    }
  }

  bool isMediaMime(String mime) {
    return mime.startsWith('image/') ||
        mime.startsWith('video/') ||
        mime.startsWith('audio/');
  }

  String mediaTypeFromMime(String mime) {
    if (mime.startsWith('image/')) return 'image';
    if (mime.startsWith('video/')) return 'video';
    if (mime.startsWith('audio/')) return 'audio';
    return 'file';
  }

  MutationResult _mutationResult({
    int? affectedCount,
    String? insertedId,
  }) {
    return MutationResult(
      affectedCount: affectedCount,
      insertedId: insertedId,
      metadata: const <String, Object?>{
        'batchSemantics': 'sequentialBestEffort',
        'implementation': 'filesystem_fallback',
      },
    );
  }

  void _ensureSupported({
    required QueryDomain domain,
    required QueryOperation operation,
  }) {
    if (!supportsDomain(domain)) {
      throw _notSupported(domain: domain, operation: operation);
    }
  }

  SimpleQueryError _notSupported({
    required QueryDomain domain,
    required QueryOperation operation,
  }) {
    return _errorBuilder(
      code: SimpleQueryErrorCode.notSupported,
      message:
          'simple_query: ${operation.name} is not supported - ${unsupportedReasonFor(domain)}',
      domain: domain,
      operation: operation,
    );
  }

  SimpleQueryError _invalidQuery(String message) {
    return _errorBuilder(
      code: SimpleQueryErrorCode.invalidQuery,
      message: message,
    );
  }

  Future<T> _guard<T>({
    required QueryDomain domain,
    required QueryOperation operation,
    required Future<T> Function() run,
  }) async {
    try {
      return await run();
    } on SimpleQueryError {
      rethrow;
    } on FileSystemException catch (error) {
      throw _errorBuilder(
        code: SimpleQueryErrorCode.unavailable,
        message: 'simple_query: filesystem fallback failed: ${error.message}',
        domain: domain,
        operation: operation,
        details: <String, Object?>{
          if (error.path != null) 'path': error.path!,
          if (error.osError?.message != null) 'osError': error.osError!.message,
        },
      );
    }
  }

  Future<bool> _applyLocalFsUpdate({
    required String rootPath,
    required String path,
    required Map<String, Object?> values,
  }) async {
    final existingFile = File(path);
    final existingDirectory = Directory(path);
    final fileExists = await existingFile.exists();
    final directoryExists = await existingDirectory.exists();
    if (!fileExists && !directoryExists) {
      return false;
    }

    final newPathRaw = values['newPath']?.toString();
    final newPath = newPathRaw != null && newPathRaw.isNotEmpty
        ? _resolveWithinRoot(
            rootPath: rootPath,
            candidatePath: newPathRaw,
            argumentName: 'values.newPath',
          )
        : null;
    var effectivePath = path;

    if (newPath != null && newPath != path) {
      if (directoryExists && !fileExists) {
        await Directory(newPath).parent.create(recursive: true);
        await existingDirectory.rename(newPath);
      } else {
        await File(newPath).parent.create(recursive: true);
        await existingFile.rename(newPath);
      }
      effectivePath = newPath;
    }

    if (directoryExists && !fileExists) {
      return newPath != null;
    }

    final targetFile = File(effectivePath);
    final bytes = _coerceBytes(values['bytes']);
    if (bytes != null) {
      await targetFile.writeAsBytes(bytes, flush: true);
      return true;
    }

    if (values.containsKey('content')) {
      await targetFile.writeAsString(values['content']?.toString() ?? '',
          flush: true);
      return true;
    }

    return newPath != null;
  }

  List<int>? _coerceBytes(Object? value) {
    if (value is List<int>) {
      return value;
    }
    if (value is List) {
      return value
          .whereType<num>()
          .map((item) => item.toInt())
          .toList(growable: false);
    }
    return null;
  }

  String _normalizeRootPath(String rootPath) {
    return p.normalize(p.absolute(rootPath));
  }

  String _resolveWithinRoot({
    required String rootPath,
    required String candidatePath,
    required String argumentName,
  }) {
    final normalizedRoot = _normalizeRootPath(rootPath);
    final resolved = p.normalize(
      p.isAbsolute(candidatePath)
          ? candidatePath
          : p.join(normalizedRoot, candidatePath),
    );
    if (p.equals(normalizedRoot, resolved) ||
        p.isWithin(normalizedRoot, resolved)) {
      return resolved;
    }
    throw _invalidQuery(
      'simple_query: $argumentName must stay within platformData.rootPath',
    );
  }
}

class PagedResult {
  const PagedResult({
    required this.records,
    required this.nextOffset,
    this.nextCursor,
  });

  final List<QueryRecord> records;
  final int? nextOffset;
  final String? nextCursor;
}
