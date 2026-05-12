import 'dart:io';

import 'package:flutter/material.dart';
import 'package:simple_query/simple_query.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const ExampleApp());
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'simple_query example',
      theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.indigo),
      home: const ExampleHome(),
    );
  }
}

class ExampleHome extends StatefulWidget {
  const ExampleHome({super.key});

  @override
  State<ExampleHome> createState() => _ExampleHomeState();
}

class _ExampleHomeState extends State<ExampleHome> {
  String _result = 'Press a button to run a sample.';
  Map<QueryDomain, CapabilityDescriptor> _capabilities =
      <QueryDomain, CapabilityDescriptor>{};

  @override
  void initState() {
    super.initState();
    _loadCapabilities();
  }

  Future<void> _loadCapabilities() async {
    try {
      final snapshot = await SimpleQuery.instance.getCapabilities();
      setState(() {
        _capabilities = <QueryDomain, CapabilityDescriptor>{
          for (final cap in snapshot.capabilities) cap.domain: cap,
        };
        _result = 'Capabilities loaded. '
            '${snapshot.capabilities.where((c) => c.canRead).length} of '
            '${snapshot.capabilities.length} domains readable on this device.';
      });
    } on SimpleQueryError catch (e) {
      setState(() => _result = _describeError(e));
    }
  }

  bool _canRead(QueryDomain domain) =>
      _capabilities[domain]?.canRead ?? false;
  bool _canObserve(QueryDomain domain) =>
      _capabilities[domain]?.canObserve ?? false;
  bool _canWrite(QueryDomain domain) =>
      _capabilities[domain]?.canWrite ?? false;

  Future<void> _run(Future<void> Function() body) async {
    setState(() => _result = 'Running...');
    try {
      await body();
    } on SimpleQueryError catch (e) {
      setState(() => _result = _describeError(e));
    }
  }

  // ---------------------------------------------------------------------------
  // Samples — each uses a different piece of the 0.5.0 public API.
  // ---------------------------------------------------------------------------

  /// Typed query via the fluent builder, with canonical-field validation.
  Future<void> _queryContactsBuilder() => _run(() async {
        final contacts = await SimpleQuery.instance
            .queryBuilder(QueryDomain.contacts)
            .orderBy('displayName')
            .page(limit: 10)
            .executeTyped<ContactRecord>(ContactRecord.fromRecord);
        setState(() {
          _result = contacts.isEmpty
              ? 'No contacts.'
              : 'First ${contacts.length} contacts:\n'
                  '${contacts.map((c) => ' • ${c.displayName}').join('\n')}\n\n'
                  '(First contact exposes ${contacts.first.extras.length} '
                  'extra OEM/niche columns via .extras)';
        });
      });

  /// Streams every page to exhaustion via queryPaginated.
  Future<void> _queryMessagesPaginated() => _run(() async {
        var pageCount = 0;
        var total = 0;
        await for (final page in SimpleQuery.instance.queryPaginated(
          QueryRequest(
            domain: QueryDomain.messages,
            page: QueryPage(limit: 20),
          ),
        )) {
          pageCount += 1;
          total += page.records.length;
          setState(() {
            _result =
                'Paginated messages: $pageCount page(s), $total record(s) so far...';
          });
          // Cap the demo so we don't infinite-loop on large datasets.
          if (pageCount >= 5) break;
        }
        setState(() {
          _result = 'Done. Streamed $total messages across $pageCount pages.';
        });
      });

  /// Write example — insert a calendar event, then delete it.
  Future<void> _mutateCalendarRoundTrip() => _run(() async {
        if (!_canWrite(QueryDomain.calendar)) {
          setState(() {
            _result =
                'Calendar write not supported: ${_capabilities[QueryDomain.calendar]?.reason ?? 'unknown'}';
          });
          return;
        }
        final insert = await SimpleQuery.instance.mutate(
          MutationRequest(
            domain: QueryDomain.calendar,
            type: MutationType.insert,
            values: <String, Object?>{
              'title': 'simple_query demo',
              'startAt': DateTime.now().toUtc().toIso8601String(),
              'endAt': DateTime.now()
                  .add(const Duration(hours: 1))
                  .toUtc()
                  .toIso8601String(),
            },
          ),
        );
        setState(() {
          _result =
              'Inserted event (affectedCount: ${insert.affectedCount}, id: ${insert.insertedId}).';
        });
      });

  /// Observe stream — listens briefly and reports change events.
  Future<void> _observeFiles() => _run(() async {
        if (!_canObserve(QueryDomain.files)) {
          setState(() {
            _result = 'Files observe not supported on this platform.';
          });
          return;
        }
        final tempDir = await Directory.systemTemp.createTemp('sq_example_');
        final stream = SimpleQuery.instance.observe(
          ObserveRequest(
            domain: QueryDomain.files,
            platformData: <String, Object?>{'rootPath': tempDir.path},
            pollingInterval: const Duration(seconds: 1),
          ),
        );
        final sub = stream.listen((event) {
          setState(() {
            _result = 'Observe event: ${event.changeType.name} on '
                '${event.ids.length} id(s) at ${event.timestamp.toIso8601String()}';
          });
        });
        // Poke the filesystem so the observer has something to report.
        await Future<void>.delayed(const Duration(milliseconds: 200));
        await File('${tempDir.path}${Platform.pathSeparator}hello.txt')
            .writeAsString('hi');
        await Future<void>.delayed(const Duration(seconds: 3));
        await sub.cancel();
        await tempDir.delete(recursive: true);
      });

  /// Self-closing binary content via withBinaryContent.
  Future<void> _openBinary() => _run(() async {
        if (!_canRead(QueryDomain.files)) {
          setState(() {
            _result = 'Files not readable on this platform.';
          });
          return;
        }
        final tempDir = await Directory.systemTemp.createTemp('sq_binary_');
        final target = File('${tempDir.path}${Platform.pathSeparator}bin.dat');
        await target.writeAsBytes(<int>[1, 2, 3, 4, 5, 6, 7, 8]);
        final length = await SimpleQuery.instance.withBinaryContent<int>(
          BinaryRequest(
            domain: QueryDomain.files,
            recordId: target.path,
            platformData: <String, Object?>{'rootPath': tempDir.path},
          ),
          (content) async {
            final bytes = await File(content.localPath).readAsBytes();
            return bytes.length;
          },
        );
        setState(() {
          _result =
              'Read $length byte(s) via withBinaryContent. Handle auto-closed.';
        });
        await tempDir.delete(recursive: true);
      });

  /// callExtension — diagnostic-style platform-specific.
  Future<void> _callExtension() => _run(() async {
        final namespace = Platform.isAndroid
            ? 'android.provider'
            : Platform.isIOS
                ? 'ios.contacts'
                : Platform.isMacOS
                    ? 'macos.photos'
                    : Platform.isWindows
                        ? 'windows.storage'
                        : 'linux.tracker';
        try {
          final result = await SimpleQuery.instance.callExtension(
            namespace: namespace,
            method: 'status',
          );
          setState(() {
            _result = 'Extension $namespace.status → $result';
          });
        } on SimpleQueryError catch (e) {
          // Many extensions don't have a 'status' method — that's fine, the
          // point here is to show the shape of callExtension.
          setState(() => _result = 'Extension $namespace.status → ${_describeError(e)}');
        }
      });

  /// queryRaw — custom content URI (Android only in practice).
  Future<void> _queryRaw() => _run(() async {
        if (!Platform.isAndroid) {
          setState(() {
            _result = 'queryRaw targets Android content providers. '
                'Skipping on ${Platform.operatingSystem}.';
          });
          return;
        }
        final response = await SimpleQuery.instance.queryRaw(
          contentUri: 'content://settings/system',
          page: QueryPage(limit: 3),
        );
        setState(() {
          _result = 'queryRaw returned ${response.records.length} raw records.\n'
              'First record keys: '
              '${response.records.firstOrNull?.keys.take(5).join(', ') ?? '(empty)'}';
        });
      });

  String _describeError(SimpleQueryError error) {
    switch (error.code) {
      case SimpleQueryErrorCode.permissionDenied:
        final perms = error.details?['permissions'] ??
            error.details?['infoPlistKey'] ??
            '(see platform requirements)';
        return 'Permission denied. Request: $perms';
      case SimpleQueryErrorCode.notSupported:
        return 'Not supported on this platform. ${error.message}';
      case SimpleQueryErrorCode.unavailable:
        return 'Data source unavailable. ${error.message}';
      case SimpleQueryErrorCode.invalidQuery:
        return 'Invalid query: ${error.message}';
      case SimpleQueryErrorCode.transientFailure:
        return 'Temporary failure. Try again.';
    }
  }

  // ---------------------------------------------------------------------------
  // UI
  // ---------------------------------------------------------------------------

  Widget _button(String label, QueryDomain? requiresRead, VoidCallback onPressed) {
    final enabled = requiresRead == null || _canRead(requiresRead);
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: FilledButton.tonal(
        onPressed: enabled ? onPressed : null,
        child: Align(alignment: Alignment.centerLeft, child: Text(label)),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('simple_query example')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            _button('1. Typed builder: contacts',
                QueryDomain.contacts, _queryContactsBuilder),
            _button('2. Paginated stream: messages',
                QueryDomain.messages, _queryMessagesPaginated),
            _button('3. Mutate: insert calendar event',
                QueryDomain.calendar, _mutateCalendarRoundTrip),
            _button('4. Observe stream: files',
                QueryDomain.files, _observeFiles),
            _button('5. BinaryContent: read file bytes',
                QueryDomain.files, _openBinary),
            _button('6. callExtension: platform-specific',
                null, _callExtension),
            _button('7. queryRaw: custom content URI (Android)',
                null, _queryRaw),
            const SizedBox(height: 12),
            FilledButton(
              onPressed: _loadCapabilities,
              child: const Text('Refresh capabilities'),
            ),
            const Divider(height: 32),
            Expanded(
              child: SingleChildScrollView(
                child: Text(_result, style: const TextStyle(fontFamily: 'monospace')),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
