# simple_query - AI Coding Agent Instructions

## Project Overview

Federated Flutter plugin providing a typed query API for device data across
Android, iOS, macOS, Windows, and Linux. Seven query domains: contacts, media,
files, calendar, messages, calls, and platformSpecific.

## Architecture

```
Monorepo (Melos workspace)
├── packages/
│   ├── simple_query/                    # App-facing facade (SimpleQuery singleton + QueryBuilder)
│   ├── simple_query_platform_interface/ # Contracts, models, records, exceptions, runtime validation
│   ├── simple_query_shared/             # Shared infrastructure (bridge, fallback, Darwin base)
│   ├── simple_query_android/            # Android backend (ContentResolver via Pigeon)
│   ├── simple_query_ios/                # iOS (CNContactStore, EventKit, FileManager)
│   ├── simple_query_macos/              # macOS (same native APIs as iOS)
│   ├── simple_query_windows/            # Windows (WinRT contacts/calendar, filesystem)
│   └── simple_query_linux/              # Linux (EDS contacts/calendar via libebook/libecal)
└── example/                             # Demo app + integration tests
```

## Key Abstractions

**Query Domains:** contacts, media, files, calendar, messages, calls, platformSpecific
**Operations:** query (read), mutate (write), batch, observe (stream changes), openBinary/closeBinary

**Capability-driven:** Always call `getCapabilities()` first. Each platform reports
what it actually supports per domain (canRead/canWrite/canObserve/canStream).
Messages and calls are Android-only (OS policy restrictions on other platforms).

## Core Types (in simple_query_platform_interface)

- `QueryRequest` — domain + filters + sort + page (with cursor support) + platformData
- `QueryResult` — records + totalCount + nextOffset + nextCursor + metadata
- `MutationRequest` — domain + type (insert/update/delete) + values + filters
- `BatchRequest` — list of MutationRequests
- `ObserveRequest` — domain + pollingInterval
- `BinaryRequest` — domain + recordId
- `QueryBuilder` — fluent API to build QueryRequest
- `SimpleQueryError` — structured error with code, message, domain, operation

**Typed Records:** `ContactRecord`, `CalendarEventRecord`, `MediaRecord`,
`FileRecord`, `MessageRecord`, `CallRecord` — each has a `fromRecord()` factory
for use with `queryTyped<T>()`.

## Platform Implementation Pattern

### Android (reference implementation)
- Maps domains to `content://` URIs
- Translates filters to parameterized SQL WHERE clauses
- Normalizes provider columns to canonical field names
- Permission checking via `simple_permissions_native`
- Binary streaming via ParcelFileDescriptor pipes
- Cursor-based pagination via `WHERE _id > ?`

### iOS/macOS (shared via simple_query_darwin)
- CNContactStore for contacts (read-only)
- EventKit for calendar (read-only)
- FileManager for files/media
- Polling-based observe with snapshot diffing
- Extension namespaces: `ios.contacts`, `ios.calendar`, `ios.photos` (and `macos.*`)

### Windows
- WinRT ContactManager for contacts (read-only)
- WinRT AppointmentManager for calendar (read-only)
- std::filesystem for files/media

### Linux
- EDS (Evolution Data Server) via libebook/libecal for contacts/calendar (read-only)
- Graceful degradation if EDS libraries not installed
- std::filesystem for files/media

### Non-Android fallback chain
All non-Android platforms use: try native bridge first → fall back to
`LocalFileSystemFallback` for files/media → report `notSupported` for
domains with no native backend.

## Pigeon Workflow

Each platform has its own Pigeon definition:
```bash
cd packages/simple_query_android && dart run pigeon --input pigeon.dart
cd packages/simple_query_ios && dart run pigeon --input pigeon.dart
# etc.
```

## Development Commands

```bash
dart run melos run analyze  # Static analysis
dart run melos run test     # All package tests
dart run melos run verify   # Both
```

## Critical Rules

1. **Field name safety:** Android filter field names are validated against
   `^[a-zA-Z_][a-zA-Z0-9_.]*$` before SQL interpolation. Never skip this.
2. **Capabilities are truth:** Never synthesize success responses for
   unsupported operations. Check `getCapabilities()` and fail with
   `notSupported` if the capability isn't there.
3. **Contract validation:** All query results pass through
   `RuntimeContractValidation.validateQueryResult()` before returning.
   Records must have required keys per domain.
4. **Path security:** Filesystem fallback validates all paths stay within
   the declared root via `_resolveWithinRoot()`.
5. **Error codes:** Use exactly 5 codes: notSupported, permissionDenied,
   unavailable, invalidQuery, transientFailure. Prefix messages with
   `simple_query:`.
