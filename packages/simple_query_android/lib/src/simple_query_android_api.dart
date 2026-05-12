import 'dart:async';
import 'dart:io';

import 'package:simple_query_platform_interface/simple_query_platform_interface.dart'
    as iface;

import 'generated/query.g.dart' as p;

class SimpleQueryAndroidApi implements p.QueryFlutterApi {
  SimpleQueryAndroidApi({
    p.QueryHostApi? hostApi,
    bool registerFlutterApi = true,
    bool enforceAndroidPlatformCheck = true,
  })  : _hostApi = hostApi ?? p.QueryHostApi(),
        _enforceAndroidPlatformCheck = enforceAndroidPlatformCheck {
    if (registerFlutterApi) {
      p.QueryFlutterApi.setUp(this);
    }
  }

  final p.QueryHostApi _hostApi;
  final bool _enforceAndroidPlatformCheck;
  int? _cachedSdkInt;

  final _observerEventsController =
      StreamController<iface.ObserveEvent>.broadcast();
  final _activeObservers = <String, _ObserverRegistration>{};
  final _openBinaryHandles = <String, String>{};

  Future<iface.CapabilitySnapshot> getCapabilities() async {
    return iface.RuntimeContractValidation.validateCapabilitySnapshot(
      const iface.CapabilitySnapshot(
        capabilities: <iface.CapabilityDescriptor>[
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.contacts,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: false,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.media,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: true,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.files,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: true,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.calendar,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: false,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.messages,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: true,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.calls,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: false,
          ),
          iface.CapabilityDescriptor(
            domain: iface.QueryDomain.platformSpecific,
            canRead: true,
            canWrite: true,
            canObserve: true,
            canStream: true,
          ),
        ],
        platformExtensions: <String, Object?>{
          'android.provider': true,
          'batchSemantics': 'providerAtomicWhenSupported',
          'queryFieldStability': <String, String>{
            'entityType': 'stable',
            'projection': 'stable',
            'platformData': 'platform_extension',
          },
        },
      ),
    );
  }

  Future<iface.QueryResult> query(iface.QueryRequest request) async {
    final contentUri = _resolveContentUri(
      domain: request.domain,
      entityType: request.entityType,
      platformData: request.platformData,
      operation: iface.QueryOperation.read,
    );

    await _ensurePermission(contentUri, write: false);

    final selection =
        _selectionFromFilters(request.filters, domain: request.domain);
    final cursor = request.page?.cursor;

    // When a cursor is present, inject a WHERE clause for cursor-based
    // pagination (WHERE _id > cursorValue) and ignore offset.
    String? effectiveSelection = selection.selection;
    List<String>? effectiveArgs = selection.args;
    int? effectiveOffset = request.page?.offset;

    if (cursor != null && cursor.isNotEmpty) {
      const cursorClause = '_id > ?';
      effectiveSelection = effectiveSelection == null
          ? cursorClause
          : '$cursorClause AND $effectiveSelection';
      effectiveArgs = <String>[cursor, ...?effectiveArgs];
      effectiveOffset = null;
    }

    final response = await _hostApi.query(
      p.QueryRequest(
        contentUri: contentUri,
        projection: _AndroidFieldAliases.translateProjection(
          domain: request.domain,
          projection: request.projection,
        ),
        selection: effectiveSelection,
        selectionArgs: effectiveArgs,
        sortOrder: _sortOrderFrom(request.sort, domain: request.domain) ??
            '_id ASC',
        limit: request.page?.limit,
        offset: effectiveOffset,
      ),
    );

    final records = _convertRows(response.rows)
        .map((row) => _normalizeRecord(domain: request.domain, row: row))
        .toList(growable: false);
    final limit = request.page?.limit;
    final hasMore = limit != null && records.length == limit;

    // Offset-based pagination.
    final currentOffset = request.page?.offset ?? 0;
    final nextOffset = hasMore ? currentOffset + records.length : null;

    // Cursor-based pagination: encode the last record's id as the next cursor.
    String? nextCursor;
    if (hasMore && records.isNotEmpty) {
      final lastId = records.last['id'];
      if (lastId != null) {
        nextCursor = lastId.toString();
      }
    }

    return iface.RuntimeContractValidation.validateQueryResult(
      domain: request.domain,
      result: iface.QueryResult(
        records: records,
        totalCount: response.rowCount,
        nextOffset: nextOffset,
        nextCursor: nextCursor,
        metadata: <String, Object?>{
          'domain': request.domain.name,
          if (request.entityType != null) 'entityType': request.entityType!,
        },
      ),
    );
  }

  Future<iface.MutationResult> mutate(iface.MutationRequest request) async {
    final contentUri = _resolveContentUri(
      domain: request.domain,
      entityType: request.entityType,
      platformData: request.platformData,
      operation: iface.QueryOperation.write,
    );
    await _ensurePermission(contentUri, write: true);

    switch (request.type) {
      case iface.MutationType.insert:
        final values = request.values;
        if (values == null || values.isEmpty) {
          throw _invalidQuery('insert mutation requires non-empty values');
        }
        final uri = await _hostApi.insert(
          p.InsertRequest(
            contentUri: contentUri,
            values: values,
          ),
        );
        return iface.MutationResult(
          affectedCount: uri == null ? 0 : 1,
          insertedId: uri,
        );
      case iface.MutationType.update:
        final values = request.values;
        if (values == null || values.isEmpty) {
          throw _invalidQuery('update mutation requires non-empty values');
        }
        final selection =
            _selectionFromFilters(request.filters, domain: request.domain);
        final count = await _hostApi.update(
          p.UpdateRequest(
            contentUri: contentUri,
            values: values,
            selection: selection.selection,
            selectionArgs: selection.args,
          ),
        );
        return iface.MutationResult(affectedCount: count);
      case iface.MutationType.delete:
        final selection =
            _selectionFromFilters(request.filters, domain: request.domain);
        final count = await _hostApi.delete(
          p.DeleteRequest(
            contentUri: contentUri,
            selection: selection.selection,
            selectionArgs: selection.args,
          ),
        );
        return iface.MutationResult(affectedCount: count);
    }
  }

  Future<iface.BatchResult> batch(iface.BatchRequest request) async {
    if (request.operations.isEmpty) {
      return const iface.BatchResult(results: <iface.MutationResult>[]);
    }

    final resolved = request.operations
        .map((operation) => _resolveContentUri(
              domain: operation.domain,
              entityType: operation.entityType,
              platformData: operation.platformData,
              operation: iface.QueryOperation.write,
            ))
        .toList(growable: false);

    for (final uri in resolved) {
      await _ensurePermission(uri, write: true);
    }

    final explicitAuthority = request.platformData?['authority'] as String?;
    final authorities = resolved.map(_authorityForUri).toSet();
    final authority = explicitAuthority ?? authorities.first;
    if (authorities.length > 1 ||
        (explicitAuthority != null &&
            authorities.single != explicitAuthority)) {
      throw _invalidQuery(
        'batch operations must resolve to a single authority',
      );
    }

    final pigeonOperations = <p.BatchOperation>[];
    for (var i = 0; i < request.operations.length; i += 1) {
      final operation = request.operations[i];
      final uri = resolved[i];
      final selection =
          _selectionFromFilters(operation.filters, domain: operation.domain);

      pigeonOperations.add(
        p.BatchOperation(
          type: _batchTypeForMutation(operation.type),
          contentUri: uri,
          values: operation.values,
          selection: selection.selection,
          selectionArgs: selection.args,
          expectedCount: operation.platformData?['expectedCount'] as int?,
          backReferenceIndex:
              operation.platformData?['backReferenceIndex'] as int?,
          backReferenceColumn:
              operation.platformData?['backReferenceColumn'] as String?,
        ),
      );
    }

    final results = await _hostApi.applyBatch(
      p.BatchRequest(authority: authority, operations: pigeonOperations),
    );

    return iface.BatchResult(
      results: results
          .whereType<p.BatchOperationResult>()
          .map(
            (item) => iface.MutationResult(
              affectedCount: item.count,
              insertedId: item.uri,
            ),
          )
          .toList(growable: false),
    );
  }

  Stream<iface.ObserveEvent> observe(iface.ObserveRequest request) {
    final contentUri = _resolveContentUri(
      domain: request.domain,
      entityType: request.entityType,
      platformData: request.platformData,
      operation: iface.QueryOperation.observe,
    );

    late final StreamController<iface.ObserveEvent> controller;
    controller = StreamController<iface.ObserveEvent>.broadcast(
      onListen: () async {
        final registration = _ObserverRegistration(
          controller: controller,
          request: request,
          contentUri: contentUri,
          hostApi: _hostApi,
          activeObservers: _activeObservers,
        );
        await registration.ensureRegistered();
      },
      onCancel: () async {
        if (controller.hasListener) return;
        final toCancel = _activeObservers.values
            .where((entry) => identical(entry.controller, controller))
            .toList(growable: false);
        for (final entry in toCancel) {
          await entry.unregister();
        }
      },
    );

    return controller.stream;
  }

  Future<iface.BinaryContentHandle> openBinary(
      iface.BinaryRequest request) async {
    final contentUri = _resolveBinaryContentUri(request);
    await _ensurePermission(contentUri, write: false);

    final descriptor = await _hostApi.openStream(contentUri);
    _openBinaryHandles[descriptor.streamId] = descriptor.streamId;

    return iface.BinaryContentHandle(
      handleId: descriptor.streamId,
      localPath: descriptor.pipePath,
      mimeType: descriptor.mimeType,
      size: descriptor.size,
      metadata: <String, Object?>{
        'domain': request.domain.name,
        if (request.entityType != null) 'entityType': request.entityType!,
        'contentUri': contentUri,
      },
    );
  }

  Future<void> closeBinary(String handleId) async {
    final streamId = _openBinaryHandles.remove(handleId) ?? handleId;
    await _hostApi.closeStream(streamId);
  }

  Future<Map<String, Object?>?> callExtension({
    required String namespace,
    required String method,
    Map<String, Object?>? args,
  }) async {
    if (namespace != 'android.provider') {
      throw iface.SimpleQueryError(
        code: iface.SimpleQueryErrorCode.notSupported,
        message:
            'simple_query: extension namespace $namespace is not supported on Android',
        operation: iface.QueryOperation.read,
      );
    }

    switch (method) {
      case 'providerCall':
        final authority = args?['authority'] as String?;
        if (authority == null || authority.isEmpty) {
          throw _invalidQuery(
              'android.provider providerCall requires authority');
        }
        final response = await _hostApi.call(
          p.ProviderCallRequest(
            authority: authority,
            method: args?['method'] as String? ?? '',
            arg: args?['arg'] as String?,
            extras: args?['extras'] as Map<String, Object?>?,
          ),
        );

        final result = response.result;
        if (result == null) return null;
        return result.map((key, value) => MapEntry(key ?? '', value));
      case 'extractToFile':
        final contentUri = args?['contentUri'] as String?;
        if (contentUri == null || contentUri.isEmpty) {
          throw _invalidQuery(
              'android.provider extractToFile requires contentUri');
        }
        await _ensurePermission(contentUri, write: false);
        final extractedPath = await _hostApi.extractToFile(contentUri);
        if (extractedPath == null || extractedPath.isEmpty) {
          throw _unavailable(
              'android.provider extractToFile returned no output path');
        }
        return <String, Object?>{
          'contentUri': contentUri,
          'path': extractedPath,
        };
      case 'queryWithJoins':
        return _queryWithJoins(args);
      default:
        throw iface.SimpleQueryError(
          code: iface.SimpleQueryErrorCode.notSupported,
          message:
              'simple_query: extension method $method is not supported in namespace $namespace',
          operation: iface.QueryOperation.read,
          domain: iface.QueryDomain.platformSpecific,
        );
    }
  }

  @override
  void onContentChange(p.ContentChangeEvent event) {
    final registration = _activeObservers[event.observerId];
    if (registration == null) return;

    final mapped = iface.ObserveEvent(
      domain: registration.request.domain,
      entityType: registration.request.entityType,
      changeType: _observeType(event.changeType),
      timestamp: DateTime.now().toUtc(),
      source: 'android.content_observer',
      metadata: <String, Object?>{
        'uri': event.uri,
        if (event.flags != null) 'flags': event.flags!,
      },
    );

    _observerEventsController.add(mapped);
    registration.controller.add(mapped);
  }

  @override
  void onStreamError(p.StreamError error) {
    _observerEventsController.add(
      iface.ObserveEvent(
        domain: iface.QueryDomain.platformSpecific,
        changeType: iface.ObserveChangeType.unknown,
        timestamp: DateTime.now().toUtc(),
        source: 'android.stream_error',
        metadata: <String, Object?>{
          'streamId': error.streamId,
          'contentUri': error.contentUri,
          'message': error.message,
          if (error.bytesTransferred != null)
            'bytesTransferred': error.bytesTransferred!,
        },
      ),
    );
  }

  Future<void> dispose() async {
    await _hostApi.unregisterAllObservers();
    for (final registration in _activeObservers.values.toList()) {
      await registration.unregister();
    }
    _activeObservers.clear();

    for (final streamId in _openBinaryHandles.values.toList(growable: false)) {
      try {
        await _hostApi.closeStream(streamId);
      } catch (_) {
        // Best-effort cleanup on dispose.
      }
    }
    _openBinaryHandles.clear();

    await _observerEventsController.close();
  }

  /// Verifies the calling app holds the OS permission(s) needed for
  /// [contentUri]. Throws `SimpleQueryError(code: permissionDenied, details:
  /// {permissions: [...], domain, operation})` when not granted.
  ///
  /// Per MEMORY.md, simple_query never *requests* permissions. The caller
  /// catches the `permissionDenied` error, requests the named permission via
  /// their own mechanism (`simple_permissions`, `permission_handler`, raw
  /// `ActivityCompat.requestPermissions`, etc.), then retries.
  Future<void> _ensurePermission(String contentUri,
      {required bool write}) async {
    if (_enforceAndroidPlatformCheck && !Platform.isAndroid) return;

    final candidates = AndroidQueryPermissionResolver.permissionsForUri(
      contentUri,
      write: write,
      sdkInt: await _androidSdkInt(),
    );
    if (candidates.isEmpty) return;

    for (final candidate in candidates) {
      final granted = await _hostApi.hasPermission(candidate);
      if (granted) return;
    }

    throw iface.SimpleQueryError(
      code: iface.SimpleQueryErrorCode.permissionDenied,
      message: candidates.length == 1
          ? 'simple_query: ${candidates.single} is required'
          : 'simple_query: one of ${candidates.join(', ')} is required',
      operation:
          write ? iface.QueryOperation.write : iface.QueryOperation.read,
      details: <String, Object?>{
        'permissions': candidates,
        'contentUri': contentUri,
        'write': write,
      },
    );
  }

  Future<int> _androidSdkInt() async {
    return _cachedSdkInt ??= await _hostApi.getAndroidSdkInt();
  }

  String _resolveContentUri({
    required iface.QueryDomain domain,
    required iface.QueryOperation operation,
    String? entityType,
    Map<String, Object?>? platformData,
  }) {
    final overrideUri = platformData?['contentUri'] as String?;
    if (overrideUri != null && overrideUri.isNotEmpty) {
      return overrideUri;
    }

    switch (domain) {
      case iface.QueryDomain.contacts:
        return 'content://com.android.contacts/contacts';
      case iface.QueryDomain.media:
        switch (entityType) {
          case 'images':
            return 'content://media/external/images/media';
          case 'videos':
            return 'content://media/external/video/media';
          case 'audio':
            return 'content://media/external/audio/media';
          default:
            return 'content://media/external/file';
        }
      case iface.QueryDomain.files:
        return 'content://media/external/file';
      case iface.QueryDomain.calendar:
        return 'content://com.android.calendar/events';
      case iface.QueryDomain.messages:
        if (entityType == 'mms') return 'content://mms';
        return 'content://sms';
      case iface.QueryDomain.calls:
        return 'content://call_log/calls';
      case iface.QueryDomain.platformSpecific:
        throw iface.SimpleQueryError(
          code: iface.SimpleQueryErrorCode.invalidQuery,
          message:
              'simple_query: platformSpecific queries require platformData.contentUri on Android',
          domain: domain,
          operation: operation,
        );
    }
  }

  String _resolveBinaryContentUri(iface.BinaryRequest request) {
    final explicitUri = request.platformData?['contentUri'] as String?;
    if (explicitUri != null && explicitUri.isNotEmpty) {
      return explicitUri;
    }

    if (request.domain == iface.QueryDomain.messages &&
        request.entityType == 'mmsPart' &&
        request.recordId != null) {
      return 'content://mms/part/${request.recordId}';
    }

    throw iface.SimpleQueryError(
      code: iface.SimpleQueryErrorCode.invalidQuery,
      message:
          'simple_query: binary request requires platformData.contentUri or messages/mmsPart recordId',
      domain: request.domain,
      operation: iface.QueryOperation.stream,
    );
  }

  static final _validFieldName = RegExp(r'^[a-zA-Z_][a-zA-Z0-9_.]*$');

  _Selection _selectionFromFilters(
    List<iface.QueryFilterCondition> filters, {
    required iface.QueryDomain domain,
  }) {
    if (filters.isEmpty) {
      return const _Selection(selection: null, args: null);
    }

    final clauses = <String>[];
    final args = <String>[];

    for (final filter in filters) {
      final field = _AndroidFieldAliases.toNativeColumn(
        domain: domain,
        canonical: filter.field,
      );
      if (!_validFieldName.hasMatch(field)) {
        throw _invalidQuery(
          'filter field name contains invalid characters: $field',
        );
      }
      final value = filter.value;
      switch (filter.operator) {
        case iface.QueryFilterOperator.equals:
          clauses.add('$field = ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.notEquals:
          clauses.add('$field != ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.greaterThan:
          clauses.add('$field > ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.greaterThanOrEqual:
          clauses.add('$field >= ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.lessThan:
          clauses.add('$field < ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.lessThanOrEqual:
          clauses.add('$field <= ?');
          args.add(value?.toString() ?? '');
        case iface.QueryFilterOperator.contains:
          clauses.add('$field LIKE ?');
          args.add('%${value?.toString() ?? ''}%');
        case iface.QueryFilterOperator.inList:
          final values = value;
          if (values is! List || values.isEmpty) {
            throw _invalidQuery(
                'inList filter requires a non-empty List value');
          }
          final placeholders =
              List<String>.filled(values.length, '?').join(', ');
          clauses.add('$field IN ($placeholders)');
          args.addAll(values.map((item) => item.toString()));
      }
    }

    return _Selection(
      selection: clauses.join(' AND '),
      args: args,
    );
  }

  String? _sortOrderFrom(
    List<iface.QuerySort> sort, {
    required iface.QueryDomain domain,
  }) {
    if (sort.isEmpty) return null;
    return sort.map(
      (item) {
        final column = _AndroidFieldAliases.toNativeColumn(
          domain: domain,
          canonical: item.field,
        );
        if (!_validFieldName.hasMatch(column)) {
          throw _invalidQuery(
            'sort field name contains invalid characters: $column',
          );
        }
        return '$column ${item.direction == iface.QuerySortDirection.descending ? 'DESC' : 'ASC'}';
      },
    ).join(', ');
  }

  iface.ObserveChangeType _observeType(p.ContentChangeType type) {
    switch (type) {
      case p.ContentChangeType.insert:
        return iface.ObserveChangeType.insert;
      case p.ContentChangeType.update:
        return iface.ObserveChangeType.update;
      case p.ContentChangeType.delete:
        return iface.ObserveChangeType.delete;
      case p.ContentChangeType.unknown:
        return iface.ObserveChangeType.unknown;
    }
  }

  List<iface.QueryRecord> _convertRows(List<Object?> rows) {
    return rows
        .whereType<Map<Object?, Object?>>()
        .map(
          (row) => row.map(
            (key, value) => MapEntry(key?.toString() ?? '', value),
          ),
        )
        .toList(growable: false);
  }

  iface.QueryRecord _normalizeRecord({
    required iface.QueryDomain domain,
    required iface.QueryRecord row,
  }) {
    switch (domain) {
      case iface.QueryDomain.files:
        final path = _firstString(
          row,
          const <String>['_data', 'path', 'file_path'],
        );
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? path ?? '',
          'path': path ?? '',
          'name':
              _firstString(row, const <String>['_display_name', 'name']) ?? '',
          'isDirectory': false,
          'size': _firstInt(row, const <String>['_size', 'size']),
          'modifiedAt':
              _firstString(row, const <String>['date_modified', 'modifiedAt']),
          'mimeType':
              _firstString(row, const <String>['mime_type', 'mimeType']),
        };
      case iface.QueryDomain.media:
        final mediaTypeRaw = _firstInt(row, const <String>['media_type']);
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? '',
          'uriOrPath':
              _firstString(row, const <String>['_data', 'uri', 'path']) ?? '',
          'mediaType': _mediaTypeFromRaw(mediaTypeRaw),
          'mimeType':
              _firstString(row, const <String>['mime_type', 'mimeType']),
          'size': _firstInt(row, const <String>['_size', 'size']),
          'createdAt':
              _firstString(row, const <String>['date_added', 'createdAt']),
          'modifiedAt':
              _firstString(row, const <String>['date_modified', 'modifiedAt']),
        };
      case iface.QueryDomain.contacts:
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? '',
          'displayName': _firstString(
                row,
                const <String>['display_name', 'displayName', 'name'],
              ) ??
              '',
          'phones': _listFromValue(row['phones']),
          'emails': _listFromValue(row['emails']),
          'organization':
              _firstString(row, const <String>['company', 'organization']),
          'updatedAt': _firstString(
              row, const <String>['contact_last_updated_timestamp']),
        };
      case iface.QueryDomain.calendar:
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? '',
          'title': _firstString(row, const <String>['title']) ?? '',
          'startAt':
              _firstString(row, const <String>['dtstart', 'startAt']) ?? '',
          'endAt': _firstString(row, const <String>['dtend', 'endAt']) ?? '',
          'isAllDay': _firstInt(row, const <String>['allDay']) == 1,
          'calendarId':
              _firstString(row, const <String>['calendar_id', 'calendarId']) ??
                  '',
          'updatedAt':
              _firstString(row, const <String>['lastDate', 'updatedAt']),
        };
      case iface.QueryDomain.messages:
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? '',
          'threadId':
              _firstString(row, const <String>['thread_id', 'threadId']),
          'address': _firstString(row, const <String>['address']),
          'body': _firstString(row, const <String>['body', 'text']),
          'timestamp': _firstString(
                row,
                const <String>['date', 'timestamp', 'normalized_date'],
              ) ??
              '',
          'direction': _smsDirection(row['type']),
          'read': _firstInt(row, const <String>['read']) == 1,
        };
      case iface.QueryDomain.calls:
        final isNewRaw = _firstInt(row, const <String>['new', 'isNew']);
        final isReadRaw = _firstInt(row, const <String>['is_read', 'isRead']);
        return <String, Object?>{
          'id': _firstString(row, const <String>['_id', 'id']) ?? '',
          'number': _firstString(row, const <String>['number']),
          'callType': _firstString(row, const <String>['type', 'callType']) ??
              'unknown',
          'durationSec':
              _firstInt(row, const <String>['duration', 'durationSec']),
          'timestamp':
              _firstString(row, const <String>['date', 'timestamp']) ?? '',
          'name': _firstString(row, const <String>['name', 'cached_name']),
          // Optional canonical fields added in 0.4.0. Absent (not false/0)
          // when the native column isn't present so consumers can detect
          // "device doesn't surface this" via `record.containsKey`.
          if (isNewRaw != null) 'isNew': isNewRaw == 1,
          if (isReadRaw != null) 'isRead': isReadRaw == 1,
          if (row.containsKey('geocoded_location') ||
              row.containsKey('geocodedLocation'))
            'geocodedLocation': _firstString(
              row,
              const <String>['geocoded_location', 'geocodedLocation'],
            ),
          if (row.containsKey('subscription_id') ||
              row.containsKey('subscriptionId'))
            'subscriptionId': _firstInt(
              row,
              const <String>['subscription_id', 'subscriptionId'],
            ),
        };
      case iface.QueryDomain.platformSpecific:
        return row;
    }
  }

  Future<Map<String, Object?>?> _queryWithJoins(
      Map<String, Object?>? args) async {
    final contentUri = args?['contentUri'] as String?;
    if (contentUri == null || contentUri.isEmpty) {
      throw _invalidQuery(
          'android.provider queryWithJoins requires contentUri');
    }

    await _ensurePermission(contentUri, write: false);

    final joinsRaw = args?['joins'];
    if (joinsRaw is! List) {
      throw _invalidQuery(
          'android.provider queryWithJoins requires joins List');
    }

    final joinSpecs = joinsRaw.map((item) {
      if (item is! Map) {
        throw _invalidQuery('join spec must be a Map<String, Object?>');
      }

      final name = item['name']?.toString();
      final joinContentUri = item['contentUri']?.toString();
      final foreignKeyColumn = item['foreignKeyColumn']?.toString();
      final parentKeyColumn = item['parentKeyColumn']?.toString();
      if (name == null ||
          joinContentUri == null ||
          foreignKeyColumn == null ||
          parentKeyColumn == null) {
        throw _invalidQuery(
          'join spec requires name, contentUri, foreignKeyColumn, parentKeyColumn',
        );
      }

      return p.JoinSpec(
        name: name,
        contentUri: joinContentUri,
        foreignKeyColumn: foreignKeyColumn,
        parentKeyColumn: parentKeyColumn,
        projection: _stringListOrNull(item['projection']),
        selection: item['selection']?.toString(),
        selectionArgs: _stringListOrNull(item['selectionArgs']),
        sortOrder: item['sortOrder']?.toString(),
      );
    }).toList(growable: false);

    final response = await _hostApi.queryWithJoins(
      p.JoinQueryRequest(
        primary: p.QueryRequest(
          contentUri: contentUri,
          projection: _stringListOrNull(args?['projection']),
          selection: args?['selection']?.toString(),
          selectionArgs: _stringListOrNull(args?['selectionArgs']),
          sortOrder: args?['sortOrder']?.toString(),
          limit: args?['limit'] as int?,
          offset: args?['offset'] as int?,
        ),
        joins: joinSpecs,
      ),
    );

    final rows = response.rows.whereType<p.JoinedRow>().map((joined) {
      final data = joined.data;
      final related = joined.related;
      final dataMap = data is Map
          ? data.map((key, value) => MapEntry(key?.toString() ?? '', value))
          : <String, Object?>{};
      final relatedMap = <String, Object?>{};
      if (related is Map) {
        for (final entry in related.entries) {
          final joinName = entry.key?.toString() ?? '';
          final value = entry.value;
          if (value is List) {
            relatedMap[joinName] = value
                .whereType<Map<Object?, Object?>>()
                .map(
                  (item) => item.map(
                      (key, value) => MapEntry(key?.toString() ?? '', value)),
                )
                .toList(growable: false);
          }
        }
      }
      return <String, Object?>{
        'data': dataMap,
        'related': relatedMap,
      };
    }).toList(growable: false);

    return <String, Object?>{
      'rowCount': response.rowCount,
      'rows': rows,
    };
  }

  List<String?>? _stringListOrNull(Object? value) {
    if (value == null) return null;
    if (value is! List) {
      throw _invalidQuery('Expected List for list argument');
    }
    return value.map((item) => item?.toString()).toList(growable: false);
  }

  String? _firstString(iface.QueryRecord row, List<String> keys) {
    for (final key in keys) {
      final value = row[key];
      if (value == null) continue;
      final text = value.toString();
      if (text.isNotEmpty) return text;
    }
    return null;
  }

  int? _firstInt(iface.QueryRecord row, List<String> keys) {
    for (final key in keys) {
      final value = row[key];
      if (value == null) continue;
      if (value is int) return value;
      final parsed = int.tryParse(value.toString());
      if (parsed != null) return parsed;
    }
    return null;
  }

  List<String> _listFromValue(Object? value) {
    if (value == null) return const <String>[];
    if (value is List) {
      return value.map((item) => item.toString()).toList(growable: false);
    }
    return <String>[value.toString()];
  }

  String _mediaTypeFromRaw(int? raw) {
    switch (raw) {
      case 1:
        return 'image';
      case 2:
        return 'audio';
      case 3:
        return 'video';
      default:
        return 'other';
    }
  }

  String _smsDirection(Object? typeValue) {
    final type = int.tryParse(typeValue?.toString() ?? '');
    switch (type) {
      case 1:
        return 'inbox';
      case 2:
        return 'sent';
      case 3:
        return 'draft';
      case 4:
        return 'outbox';
      default:
        return 'unknown';
    }
  }

  String _authorityForUri(String contentUri) {
    final parsed = Uri.parse(contentUri);
    return parsed.authority;
  }

  p.BatchOperationType _batchTypeForMutation(iface.MutationType type) {
    switch (type) {
      case iface.MutationType.insert:
        return p.BatchOperationType.insert;
      case iface.MutationType.update:
        return p.BatchOperationType.update;
      case iface.MutationType.delete:
        return p.BatchOperationType.delete;
    }
  }

  iface.SimpleQueryError _invalidQuery(String message) {
    return iface.SimpleQueryError(
      code: iface.SimpleQueryErrorCode.invalidQuery,
      message: 'simple_query: $message',
    );
  }

  iface.SimpleQueryError _unavailable(String message) {
    return iface.SimpleQueryError(
      code: iface.SimpleQueryErrorCode.unavailable,
      message: 'simple_query: $message',
    );
  }
}

/// Maps an Android content URI to the OS permission(s) that gate it.
///
/// Returns fully-qualified Android permission strings (e.g.
/// `android.permission.READ_CONTACTS`) — the same identifiers the caller
/// would pass to `ContextCompat.checkSelfPermission` or
/// `ActivityCompat.requestPermissions`.
///
/// Multiple permissions in the list represent *alternatives*: if any one is
/// granted, the query proceeds. This covers the Android 13 media-permission
/// split, where API 33+ wants `READ_MEDIA_IMAGES` / `_VIDEO` / `_AUDIO` while
/// API ≤32 still uses `READ_EXTERNAL_STORAGE`.
///
/// Returns an empty list for content URIs the provider does not gate
/// (e.g. write paths on media and SMS, which the system never exposes for
/// third-party writes).
///
/// simple_query never *requests* permissions — that is the caller's job.
/// This catalog tells them which identifier to request after seeing a
/// `permissionDenied` error.
abstract final class AndroidQueryPermissionResolver {
  static const _readContacts = 'android.permission.READ_CONTACTS';
  static const _writeContacts = 'android.permission.WRITE_CONTACTS';
  static const _readSms = 'android.permission.READ_SMS';
  static const _readCallLog = 'android.permission.READ_CALL_LOG';
  static const _writeCallLog = 'android.permission.WRITE_CALL_LOG';
  static const _readCalendar = 'android.permission.READ_CALENDAR';
  static const _writeCalendar = 'android.permission.WRITE_CALENDAR';
  static const _readExternalStorage =
      'android.permission.READ_EXTERNAL_STORAGE';
  static const _readMediaImages = 'android.permission.READ_MEDIA_IMAGES';
  static const _readMediaVideo = 'android.permission.READ_MEDIA_VIDEO';
  static const _readMediaAudio = 'android.permission.READ_MEDIA_AUDIO';

  static List<String> permissionsForUri(
    String contentUri, {
    required bool write,
    required int sdkInt,
  }) {
    final uri = Uri.parse(contentUri);
    final authority = uri.authority.toLowerCase();
    final path = uri.path.toLowerCase();

    if (authority == 'sms' || authority == 'mms' || authority == 'mms-sms') {
      return write ? const <String>[] : const <String>[_readSms];
    }

    if (authority.contains('contacts') ||
        authority.contains('com.android.contacts')) {
      return <String>[write ? _writeContacts : _readContacts];
    }

    if (authority == 'call_log' || path.contains('call_log')) {
      return <String>[write ? _writeCallLog : _readCallLog];
    }

    if (authority.contains('calendar') ||
        authority.contains('com.android.calendar')) {
      return <String>[write ? _writeCalendar : _readCalendar];
    }

    if (authority.contains('media') ||
        authority.contains('com.android.providers.media')) {
      if (write) return const <String>[];
      // API 33+ introduced the granular READ_MEDIA_* permissions. On older
      // devices, fall back to READ_EXTERNAL_STORAGE. Returning both as
      // alternatives lets the device decide: `hasPermission` returns false
      // for the modern name on API ≤32 (not in manifest) and false for the
      // legacy name on API 33+ (ignored).
      if (path.contains('/images/')) {
        return sdkInt >= 33
            ? const <String>[_readMediaImages]
            : const <String>[_readExternalStorage];
      }
      if (path.contains('/video/')) {
        return sdkInt >= 33
            ? const <String>[_readMediaVideo]
            : const <String>[_readExternalStorage];
      }
      if (path.contains('/audio/')) {
        return sdkInt >= 33
            ? const <String>[_readMediaAudio]
            : const <String>[_readExternalStorage];
      }
      // Generic media URI (no kind in path) — on API 33+ there is no single
      // umbrella permission; accept any of the three.
      return sdkInt >= 33
          ? const <String>[_readMediaImages, _readMediaVideo, _readMediaAudio]
          : const <String>[_readExternalStorage];
    }

    return const <String>[];
  }
}

/// Canonical-to-Android-column translation for filters, sort, and
/// projection. Consumers of simple_query pass canonical field names
/// (`callType`, `timestamp`, `durationSec`, ...); the Android ContentResolver
/// uses its own column names (`type`, `date`, `duration`, ...). This helper
/// keeps the two vocabularies connected in one place.
///
/// Output records are normalised back to canonical keys by
/// [SimpleQueryAndroidApi._normalizeRecord], which reads native columns
/// directly. This table is the inverse — used before the native call when
/// the caller supplies a field name in filters / sort / projection.
///
/// For [iface.QueryDomain.platformSpecific] no translation happens — field
/// names pass through unchanged. For any named domain, an unknown canonical
/// field throws `SimpleQueryError(invalidQuery)` via
/// [iface.QueryFieldCatalog.ensureKnown].
abstract final class _AndroidFieldAliases {
  static const Map<iface.QueryDomain, Map<String, String>> _aliases =
      <iface.QueryDomain, Map<String, String>>{
    iface.QueryDomain.contacts: <String, String>{
      'id': '_id',
      'displayName': 'display_name',
      'organization': 'company',
      'updatedAt': 'contact_last_updated_timestamp',
      // 'phones' / 'emails' are not single-column on Android contacts —
      // filtering/sorting by them requires a join. Callers that need this
      // should use callExtension('android.provider', 'queryWithJoins') or
      // QueryDomain.platformSpecific with a raw contentUri.
    },
    iface.QueryDomain.calendar: <String, String>{
      'id': '_id',
      'title': 'title',
      'startAt': 'dtstart',
      'endAt': 'dtend',
      'isAllDay': 'allDay',
      'calendarId': 'calendar_id',
      'updatedAt': 'lastDate',
    },
    iface.QueryDomain.media: <String, String>{
      'id': '_id',
      'uriOrPath': '_data',
      'mediaType': 'media_type',
      'mimeType': 'mime_type',
      'size': '_size',
      'createdAt': 'date_added',
      'modifiedAt': 'date_modified',
    },
    iface.QueryDomain.files: <String, String>{
      'id': '_id',
      'path': '_data',
      'name': '_display_name',
      'size': '_size',
      'mimeType': 'mime_type',
      'modifiedAt': 'date_modified',
      'modifiedEpochMs': 'date_modified',
    },
    iface.QueryDomain.messages: <String, String>{
      'id': '_id',
      'threadId': 'thread_id',
      'address': 'address',
      'body': 'body',
      'timestamp': 'date',
      'read': 'read',
      // 'direction' is canonical-only; it derives from native `type` with a
      // value-level mapping. Filtering/sorting by direction is not yet
      // supported — callers should filter by `type` via platformSpecific.
    },
    iface.QueryDomain.calls: <String, String>{
      'id': '_id',
      'number': 'number',
      'callType': 'type',
      'durationSec': 'duration',
      'timestamp': 'date',
      'name': 'name',
      'isNew': 'new',
      'isRead': 'is_read',
      'geocodedLocation': 'geocoded_location',
      'subscriptionId': 'subscription_id',
    },
  };

  /// Translates [canonical] into the Android ContentResolver column name for
  /// [domain]. Validates that [canonical] is a known canonical field first
  /// (throws `invalidQuery` if not). If no explicit alias is defined, the
  /// canonical name is returned unchanged — reasonable for fields that match
  /// their native column (e.g. `number` on calls, `address` on messages).
  static String toNativeColumn({
    required iface.QueryDomain domain,
    required String canonical,
  }) {
    if (domain == iface.QueryDomain.platformSpecific) return canonical;
    iface.QueryFieldCatalog.ensureKnown(
      domain: domain,
      canonical: canonical,
    );
    return _aliases[domain]?[canonical] ?? canonical;
  }

  /// Translates a projection list: each entry is validated and mapped to its
  /// native column. Returns null (pass-through) for null/empty projections
  /// and platformSpecific domains.
  static List<String>? translateProjection({
    required iface.QueryDomain domain,
    required List<String>? projection,
  }) {
    if (projection == null || projection.isEmpty) return projection;
    if (domain == iface.QueryDomain.platformSpecific) return projection;
    return <String>[
      for (final field in projection)
        toNativeColumn(domain: domain, canonical: field),
    ];
  }
}

class _Selection {
  const _Selection({
    required this.selection,
    required this.args,
  });

  final String? selection;
  final List<String>? args;
}

class _ObserverRegistration {
  _ObserverRegistration({
    required this.controller,
    required this.request,
    required this.contentUri,
    required this.hostApi,
    required this.activeObservers,
  });

  final StreamController<iface.ObserveEvent> controller;
  final iface.ObserveRequest request;
  final String contentUri;
  final p.QueryHostApi hostApi;
  final Map<String, _ObserverRegistration> activeObservers;

  String? _observerId;
  Future<void>? _inFlightRegistration;

  Future<void> ensureRegistered() async {
    if (_observerId != null || _inFlightRegistration != null) return;

    final future = _register();
    _inFlightRegistration = future;
    try {
      await future;
    } finally {
      if (identical(_inFlightRegistration, future)) {
        _inFlightRegistration = null;
      }
    }
  }

  Future<void> _register() async {
    final observerId = await hostApi.registerObserver(
      p.ObserverRequest(
        contentUri: contentUri,
        notifyForDescendants: true,
      ),
    );
    _observerId = observerId;
    activeObservers[observerId] = this;

    if (!controller.hasListener) {
      await unregister();
    }
  }

  Future<void> unregister() async {
    final observerId = _observerId;
    if (observerId == null) return;
    _observerId = null;
    activeObservers.remove(observerId);
    await hostApi.unregisterObserver(observerId);
  }
}
