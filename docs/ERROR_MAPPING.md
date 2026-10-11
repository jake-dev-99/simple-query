# Error Mapping Policy (v2)

All platform implementations must convert native/transport failures into
`SimpleQueryError` before returning to app-facing callers.

## Required code mapping

- `notSupported`
  - Operation/domain is restricted by OS policy or intentionally unavailable.
- `permissionDenied`
  - Runtime permission, entitlement, or policy denial.
- `unavailable`
  - Backend/provider/resource is missing or not reachable.
- `invalidQuery`
  - Invalid request shape, unsupported filter usage, malformed args.
- `transientFailure`
  - Retriable platform failure (I/O race, temporary provider failure, etc).

## Guardrails

- No raw `PlatformException`, `MissingPluginException`, or host transport error
  may cross the package boundary.
- Include domain/operation where known.
- Include stable human-readable message prefixes (`simple_query:`).

## Platform notes

### Android host errors

Every Pigeon host call is mapped: queries, joins, mutations, batches,
binary open/close, provider calls, file extraction, observer
registration, and the permission pre-flight. The Kotlin host classifies
each failure by exception type (subclasses land with their parent) and
sends one of the bridge codes `permission-denied`, `invalid-query`,
`unavailable`, `transient` or `not-supported`. The Dart side maps that
code to a `SimpleQueryError`.

| Host failure | Code |
| --- | --- |
| `SecurityException` raised by the provider | `permissionDenied` |
| `IllegalArgumentException` (e.g. `Unknown URI`) | `invalidQuery` |
| `UnsupportedOperationException` | `notSupported` |
| `SQLiteConstraintException` | `invalidQuery` |
| `SQLiteFullException`, `SQLiteDiskIOException`, `SQLiteDatabaseLockedException`, `SQLiteTableLockedException` | `transientFailure` |
| `SQLiteDatabaseCorruptException` | `unavailable` |
| Any other `SQLiteException` (e.g. syntax, `no such column`) | `invalidQuery` |
| `OperationApplicationException` | `invalidQuery` |
| `OperationCanceledException` | `transientFailure` |
| `FileNotFoundException` | `unavailable` |
| `RemoteException` (incl. `DeadObjectException`) | `unavailable` |
| Null cursor from `ContentResolver.query(...)` | `unavailable` |
| Pigeon `channel-error` (no host answered) | `unavailable` |
| Pigeon `null-error`, or any other failure or code | `transientFailure` |

A failure no rule covers maps to `transientFailure`, so `unavailable`
keeps meaning only "provider unreachable".

Each mapped error carries:

- the request's domain and operation where known;
- a `simple_query:` message (the host's own, or one naming the call
  target and host code);
- `details`: `code` (the host code), `details` (the host's details,
  including `nativeStackTrace` for a classified exception), plus
  `contentUri` and `nativeType` (the Java class name) when known.

The original stack trace is kept.

A null cursor maps to `unavailable` on the `query()` and
`android.provider` `queryWithJoins` paths, never to an empty result. Its
`details.contentUri` is the failing provider's: a related query's for a
join. See
[API_SEMANTICS.md](API_SEMANTICS.md#null-cursor-semantics-android).
