# Migration from Raw ContentProvider Code

If you previously used Android ContentProvider queries directly (via
`ContentResolver.query()`, raw `content://` URIs, SQL selection strings),
this guide shows how to express the same operations with `simple_query`.

## Core Changes

| Before (raw ContentProvider) | After (simple_query) |
| --- | --- |
| URI/SQL-shaped request objects (`contentUri`, `selection`, `selectionArgs`, `sortOrder`) | Typed contracts (`QueryRequest`, `MutationRequest`, `BatchRequest`) |
| Implicit platform assumptions | Capability-first via `getCapabilities()` |
| Mixed exception/transport errors | Normalized `SimpleQueryError` with `SimpleQueryErrorCode` |
| Android-only | Cross-platform (Android, iOS, macOS, Windows, Linux) |

## Operation Mapping

| Raw ContentProvider | simple_query |
| --- | --- |
| `contentResolver.query(uri, projection, selection, ...)` | `SimpleQuery.instance.query(QueryRequest(domain: ..., filters: ..., projection: ..., sort: ..., page: ...))` |
| `contentResolver.insert(uri, values)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.insert, ...))` |
| `contentResolver.update(uri, values, selection, ...)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.update, ...))` |
| `contentResolver.delete(uri, selection, ...)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.delete, ...))` |
| `contentResolver.applyBatch(...)` | `SimpleQuery.instance.batch(BatchRequest(...))` |
| `contentResolver.registerContentObserver(...)` | `SimpleQuery.instance.observe(ObserveRequest(domain: ...))` |
| `contentResolver.openInputStream(uri)` | `SimpleQuery.instance.openBinary(BinaryRequest(...))` |
| Provider-specific `call()` methods | `SimpleQuery.instance.callExtension(namespace: ..., method: ..., args: ...)` |

## URI to Domain

| Content URI | QueryDomain |
| --- | --- |
| `content://com.android.contacts/...` | `QueryDomain.contacts` |
| `content://media/...` | `QueryDomain.media` |
| `content://sms`, `content://mms` | `QueryDomain.messages` |
| `content://call_log/calls` | `QueryDomain.calls` |
| `content://com.android.calendar/...` | `QueryDomain.calendar` |
| File/document paths | `QueryDomain.files` |

## Required Pattern

```dart
// 1. Check what the current platform supports.
final caps = await SimpleQuery.instance.getCapabilities();

// 2. Gate features by capability.
final contacts = caps.capabilities.firstWhere((c) => c.domain == QueryDomain.contacts);
if (!contacts.canRead) {
  // Show user-friendly message using contacts.reason
}

// 3. Query with typed filters instead of raw SQL.
final result = await SimpleQuery.instance.query(
  QueryRequest(
    domain: QueryDomain.contacts,
    filters: [QueryFilterCondition(field: 'displayName', operator: QueryFilterOperator.contains, value: 'Alice')],
    page: QueryPage(limit: 20),
  ),
);

// 4. Use typed records for safe field access.
final contacts = result.records.map(ContactRecord.fromRecord).toList();
```
