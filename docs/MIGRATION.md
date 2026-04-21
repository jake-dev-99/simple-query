# Migration Guide

This guide shows how to express device-data operations you'd normally
write against a platform's native API using `simple_query`. Pick the
section that matches your starting point.

- [From raw Android ContentProvider](#from-raw-android-contentprovider)
- [From iOS / macOS Contacts + EventKit + Photos](#from-ios--macos-contacts--eventkit--photos)
- [From desktop filesystem code (Linux / Windows / macOS files)](#from-desktop-filesystem-code)
- [Required pattern (all platforms)](#required-pattern-all-platforms)

---

## From raw Android ContentProvider

If you previously used `ContentResolver.query()`, raw `content://` URIs,
and SQL selection strings, this is the mapping.

### Core shape changes

| Before (raw ContentProvider) | After (simple_query) |
| --- | --- |
| URI/SQL-shaped request objects (`contentUri`, `selection`, `selectionArgs`, `sortOrder`) | Typed contracts (`QueryRequest`, `MutationRequest`, `BatchRequest`) |
| Implicit platform assumptions | Capability-first via `getCapabilities()` |
| Mixed exception/transport errors | Normalized `SimpleQueryError` with `SimpleQueryErrorCode` |
| Android-only | Cross-platform (Android, iOS, macOS, Windows, Linux) |

### Operation mapping

| Raw ContentProvider | simple_query |
| --- | --- |
| `contentResolver.query(uri, projection, selection, ...)` | `SimpleQuery.instance.query(QueryRequest(domain: ..., filters: ..., projection: ..., sort: ..., page: ...))` |
| `contentResolver.insert(uri, values)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.insert, ...))` |
| `contentResolver.update(uri, values, selection, ...)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.update, ...))` |
| `contentResolver.delete(uri, selection, ...)` | `SimpleQuery.instance.mutate(MutationRequest(type: MutationType.delete, ...))` |
| `contentResolver.applyBatch(...)` | `SimpleQuery.instance.batch(BatchRequest(...))` |
| `contentResolver.registerContentObserver(...)` | `SimpleQuery.instance.observe(ObserveRequest(domain: ...))` |
| `contentResolver.openInputStream(uri)` | `SimpleQuery.instance.withBinaryContent(request, body)` |
| Provider-specific `call()` methods | `SimpleQuery.instance.callExtension(namespace: ..., method: ..., args: ...)` |
| Custom provider (`content://com.biz.app/...`) | `SimpleQuery.instance.queryRaw(contentUri: ...)` |

### URI to QueryDomain

| Content URI | QueryDomain |
| --- | --- |
| `content://com.android.contacts/...` | `QueryDomain.contacts` |
| `content://media/...` | `QueryDomain.media` |
| `content://sms`, `content://mms` | `QueryDomain.messages` |
| `content://call_log/calls` | `QueryDomain.calls` |
| `content://com.android.calendar/...` | `QueryDomain.calendar` |
| File/document paths | `QueryDomain.files` |
| Anything else | `queryRaw(contentUri: ...)` or `QueryDomain.platformSpecific` |

### Column name to canonical field

Since 0.4.0, filter/sort/projection fields on named domains use
**canonical names**, not raw column names. Common mappings:

| Raw Android column | Canonical name (on named domain) |
| --- | --- |
| `_id` | `id` |
| `type` (calls) | `callType` |
| `duration` (calls) | `durationSec` |
| `date` (calls, messages) | `timestamp` |
| `display_name` (contacts) | `displayName` |
| `dtstart`, `dtend` (calendar) | `startAt`, `endAt` |
| `_data` (media, files) | `uriOrPath` (media) / `path` (files) |
| `_display_name` (files) | `name` |
| `_size` | `size` |
| `new` (calls) | `isNew` |
| `is_read` (calls) | `isRead` |
| `geocoded_location` (calls) | `geocodedLocation` |
| `subscription_id` (calls) | `subscriptionId` |

Full catalog: `QueryFieldCatalog.canonicalFields(domain)`. Passing a raw
column name on a named domain throws `SimpleQueryError(invalidQuery,
details: {field, allowed})` — so typos surface immediately.

Need raw column access? Use `QueryDomain.platformSpecific` with a
`platformData['contentUri']`, or (preferred) `SimpleQuery.queryRaw`.

---

## From iOS / macOS Contacts + EventKit + Photos

| Before (native) | After (simple_query) |
| --- | --- |
| `CNContactStore().enumerateContacts(with:)` | `SimpleQuery.instance.query(QueryRequest(domain: QueryDomain.contacts))` |
| `CNContactStore.authorizationStatus(for: .contacts)` | Implicit — the query throws `SimpleQueryError(permissionDenied, details: {framework: 'Contacts', infoPlistKey: 'NSContactsUsageDescription'})` when not granted |
| `EKEventStore().events(matching: predicate)` | `SimpleQuery.instance.query(QueryRequest(domain: QueryDomain.calendar, platformData: {'startAt': isoStart, 'endAt': isoEnd}))` |
| `EKEventStore.authorizationStatus(for: .event)` | Implicit (same shape as contacts) |
| `PHAsset.fetchAssets(with:)` | `SimpleQuery.instance.query(QueryRequest(domain: QueryDomain.media, platformData: {'rootPath': dir}))` or the `ios.photos` extension namespace |
| `CNContactStore().containers(matching:)` | `callExtension(namespace: 'ios.contacts', method: 'listContainers')` — see `docs/extensions/ios.md` |

**Read-only caveat.** Contacts and calendar on iOS/macOS are **read-only**
through `simple_query` today. Writes raise `SimpleQueryError.notSupported`
with a reason string. Capability reporting (`getCapabilities`) reflects
this — check `canWrite` before attempting.

**Messages and calls are not exposed** by Apple platforms. Both domains
return `notSupported` on iOS and macOS (OS policy, not a simple_query
limitation).

**Permissions flow:**

```dart
try {
  final result = await SimpleQuery.instance.query(
    const QueryRequest(domain: QueryDomain.contacts),
  );
  // handle result
} on SimpleQueryError catch (e) when (e.code == SimpleQueryErrorCode.permissionDenied) {
  // e.details: {'framework': 'Contacts', 'infoPlistKey': 'NSContactsUsageDescription',
  //             'authorizationStatus': 'notDetermined' | 'denied' | ...}
  // 1. Ensure the Info.plist key is declared.
  // 2. Request via your preferred mechanism (permission_handler,
  //    simple_permissions, or direct CNContactStore.requestAccess).
  // 3. Retry the query.
}
```

---

## From desktop filesystem code

`QueryDomain.files` and `QueryDomain.media` on Linux, Windows, and
macOS-fallback paths walk an arbitrary root directory. There's no system
provider to migrate away from; the mapping is from raw `Directory` /
`File` code to typed queries.

| Before (raw filesystem) | After (simple_query) |
| --- | --- |
| `Directory(path).list()` + per-entry filtering | `SimpleQuery.instance.query(QueryRequest(domain: QueryDomain.files, platformData: {'rootPath': path}, filters: [...]))` |
| `FileStat.stat(path)` | Reads `FileRecord.size` / `.modifiedEpochMs` from query results |
| `File(path).readAsBytes()` after content URI resolution | `SimpleQuery.instance.withBinaryContent(request, (c) => File(c.localPath).readAsBytes())` |
| `DirectoryWatcher(path).events` | `SimpleQuery.instance.observe(ObserveRequest(domain: QueryDomain.files, platformData: {'rootPath': path}))` |

**`platformData.rootPath` is required** for non-Android `files` and
`media` queries. Omitting it throws `SimpleQueryError(invalidQuery,
details: {'missing': 'platformData.rootPath'})`.

**Permission mapping on desktop:**

| Native signal | simple_query error |
| --- | --- |
| `FileSystemException(EPERM)` / `EACCES` / `ERROR_ACCESS_DENIED` | `SimpleQueryError(permissionDenied, details: {path, osError, errno})` |
| Other `FileSystemException` | `SimpleQueryError(unavailable, details: {path, osError})` |

macOS app sandbox entitlements (`com.apple.security.files.user-selected.read-only`
etc.) are still the caller's responsibility — simple_query can't grant
them. A denied sandbox access surfaces as `permissionDenied`.

---

## Required pattern (all platforms)

```dart
// 1. Check what the current platform supports.
final caps = await SimpleQuery.instance.getCapabilities();
final contacts = caps.capabilities.firstWhere(
  (c) => c.domain == QueryDomain.contacts,
);
if (!contacts.canRead) {
  // Use contacts.reason for the user-facing message.
  return;
}

// 2. Query with typed filters on canonical fields.
try {
  final result = await SimpleQuery.instance.query(
    QueryRequest(
      domain: QueryDomain.contacts,
      filters: [
        QueryFilterCondition(
          field: 'displayName', // canonical, not raw column name
          operator: QueryFilterOperator.contains,
          value: 'Alice',
        ),
      ],
      page: QueryPage(limit: 20),
    ),
  );

  // 3. Use typed records for safe field access.
  final contacts =
      result.records.map(ContactRecord.fromRecord).toList();
  for (final contact in contacts) {
    print('${contact.displayName} — ${contact.phones.join(', ')}');
    // OEM-specific columns are available via contact.extras.
  }
} on SimpleQueryError catch (e) {
  switch (e.code) {
    case SimpleQueryErrorCode.permissionDenied:
      // e.details names the OS permission to request.
      break;
    case SimpleQueryErrorCode.notSupported:
      // Domain genuinely unavailable on this platform.
      break;
    case SimpleQueryErrorCode.invalidQuery:
      // Bug in your request — check e.details.
      break;
    case SimpleQueryErrorCode.unavailable:
      // Backend temporarily down / path missing — retry or fall back.
      break;
    case SimpleQueryErrorCode.transientFailure:
      // Retry.
      break;
  }
}
```

See [DESIGN.md](DESIGN.md) for the principles behind this shape.
