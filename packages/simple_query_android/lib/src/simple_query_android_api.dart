import 'dart:async';
import 'dart:io';

import 'package:simple_permissions_native/simple_permissions_native.dart';
import 'package:simple_query_platform_interface/simple_query_platform_interface.dart'
    as iface;

import 'generated/query.g.dart' as p;

class SimpleQueryAndroidApi implements p.QueryFlutterApi {
  SimpleQueryAndroidApi({
    p.QueryHostApi? hostApi,
    Future<PermissionGrant> Function(Permission permission)? checkPermission,
    bool registerFlutterApi = true,
    bool enforceAndroidPlatformCheck = true,
  })  : _hostApi = hostApi ?? p.QueryHostApi(),
        _checkPermission =
            checkPermission ?? SimplePermissionsNative.instance.check,
        _enforceAndroidPlatformCheck = enforceAndroidPlatformCheck {
    if (registerFlutterApi) {
      p.QueryFlutterApi.setUp(this);
    }
  }

  final p.QueryHostApi _hostApi;
  final Future<PermissionGrant> Function(Permission permission)
      _checkPermission;
  final bool _enforceAndroidPlatformCheck;

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

    final selection = _selectionFromFilters(request.filters);
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
        projection: request.projection,
        selection: effectiveSelection,
        selectionArgs: effectiveArgs,
        sortOrder: _sortOrderFrom(request.sort) ?? '_id ASC',
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
        final selection = _selectionFromFilters(request.filters);
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
        final selection = _selectionFromFilters(request.filters);
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
      final selection = _selectionFromFilters(operation.filters);

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

  Future<void> _ensurePermission(String contentUri,
      {required bool write}) async {
    if (_enforceAndroidPlatformCheck && !Platform.isAndroid) return;

    final permission = AndroidQueryPermissionResolver.permissionForUri(
      contentUri,
      write: write,
    );
    if (permission == null) return;

    final grant = await _checkPermission(permission);
    if (grant != PermissionGrant.granted && grant != PermissionGrant.limited) {
      throw iface.SimpleQueryError(
        code: iface.SimpleQueryErrorCode.permissionDenied,
        message:
            'simple_query: permission ${permission.identifier} is required',
        operation:
            write ? iface.QueryOperation.write : iface.QueryOperation.read,
      );
    }
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

  _Selection _selectionFromFilters(List<iface.QueryFilterCondition> filters) {
    if (filters.isEmpty) {
      return const _Selection(selection: null, args: null);
    }

    final clauses = <String>[];
    final args = <String>[];

    for (final filter in filters) {
      final field = filter.field;
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

  String? _sortOrderFrom(List<iface.QuerySort> sort) {
    if (sort.isEmpty) return null;
    return sort.map(
      (item) {
        if (!_validFieldName.hasMatch(item.field)) {
          throw _invalidQuery(
            'sort field name contains invalid characters: ${item.field}',
          );
        }
        return '${item.field} ${item.direction == iface.QuerySortDirection.descending ? 'DESC' : 'ASC'}';
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
                .whereType<Map>()
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

abstract final class AndroidQueryPermissionResolver {
  static Permission? permissionForUri(String contentUri,
      {required bool write}) {
    final uri = Uri.parse(contentUri);
    final authority = uri.authority.toLowerCase();
    final path = uri.path.toLowerCase();

    if (authority == 'sms' || authority == 'mms' || authority == 'mms-sms') {
      return write ? null : const ReadSms();
    }

    if (authority.contains('contacts') ||
        authority.contains('com.android.contacts')) {
      return write ? const WriteContacts() : const ReadContacts();
    }

    if (authority == 'call_log' || path.contains('call_log')) {
      return write ? const WriteCallLog() : const ReadCallLog();
    }

    if (authority.contains('calendar') ||
        authority.contains('com.android.calendar')) {
      return write ? const WriteCalendar() : const ReadCalendar();
    }

    if (authority.contains('media') ||
        authority.contains('com.android.providers.media')) {
      if (write) return null;
      if (path.contains('/images/')) return const VersionedPermission.images();
      if (path.contains('/video/')) return const VersionedPermission.video();
      if (path.contains('/audio/')) return const VersionedPermission.audio();
      return const ReadExternalStorage();
    }

    return null;
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
