import 'package:flutter/material.dart';
import 'package:simple_query/simple_query.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const ExampleApp());
}

class ExampleApp extends StatefulWidget {
  const ExampleApp({super.key});

  @override
  State<ExampleApp> createState() => _ExampleAppState();
}

class _ExampleAppState extends State<ExampleApp> {
  String _result = 'Press a button to run a sample query.';
  Map<QueryDomain, CapabilityDescriptor> _capabilities = {};

  @override
  void initState() {
    super.initState();
    _loadCapabilities();
  }

  Future<void> _loadCapabilities() async {
    final snapshot = await SimpleQuery.instance.getCapabilities();
    setState(() {
      _capabilities = {
        for (final cap in snapshot.capabilities) cap.domain: cap,
      };
    });
  }

  bool _canRead(QueryDomain domain) {
    return _capabilities[domain]?.canRead ?? false;
  }

  Future<void> _queryContacts() async {
    if (!_canRead(QueryDomain.contacts)) {
      setState(() {
        _result =
            'Contacts not available: ${_capabilities[QueryDomain.contacts]?.reason ?? 'unknown'}';
      });
      return;
    }

    try {
      final response = await SimpleQuery.instance.query(
        const QueryRequest(
          domain: QueryDomain.contacts,
          page: QueryPage(limit: 5),
        ),
      );
      final contacts = response.records.map(ContactRecord.fromRecord).toList();
      setState(() {
        _result = contacts.isEmpty
            ? 'No contacts found.'
            : contacts.map((c) => c.displayName).join(', ');
      });
    } on SimpleQueryError catch (e) {
      setState(() => _result = _describeError(e));
    }
  }

  Future<void> _queryMessages() async {
    if (!_canRead(QueryDomain.messages)) {
      setState(() {
        _result =
            'Messages not available: ${_capabilities[QueryDomain.messages]?.reason ?? 'unknown'}';
      });
      return;
    }

    try {
      final response = await SimpleQuery.instance.query(
        const QueryRequest(
          domain: QueryDomain.messages,
          page: QueryPage(limit: 5),
        ),
      );
      setState(() {
        _result = 'Messages query returned ${response.records.length} records.';
      });
    } on SimpleQueryError catch (e) {
      setState(() => _result = _describeError(e));
    }
  }

  String _describeError(SimpleQueryError error) {
    switch (error.code) {
      case SimpleQueryErrorCode.permissionDenied:
        return 'Permission denied. Grant the required permission and try again.';
      case SimpleQueryErrorCode.notSupported:
        return 'Not supported on this platform.';
      case SimpleQueryErrorCode.unavailable:
        return 'Data source unavailable.';
      case SimpleQueryErrorCode.invalidQuery:
        return 'Invalid query: ${error.message}';
      case SimpleQueryErrorCode.transientFailure:
        return 'Temporary failure. Try again.';
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('simple_query Example')),
        body: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              ElevatedButton(
                onPressed: _queryContacts,
                child: const Text('Query Contacts'),
              ),
              const SizedBox(height: 8),
              ElevatedButton(
                onPressed: _queryMessages,
                child: const Text('Query Messages (Android only)'),
              ),
              const SizedBox(height: 16),
              Expanded(
                child: SingleChildScrollView(
                  child: Text(_result),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
