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
| No stream from `openInputStream(...)` when `openBinary()` opens content | `unavailable` |
| No provider installed for a write, batch or provider call | `unavailable` |
| Pigeon `channel-error` (no host answered) | `unavailable` |
| Pigeon `null-error`, or any other failure or code (including a `java.lang.Error`) | `transientFailure` |

A failure no rule covers maps to `transientFailure`, so `unavailable`
keeps meaning only "provider unreachable".

Writes, batches and provider calls acquire the provider first, so a
missing provider is `unavailable` there too (details `reason:
noProvider`), never the `invalidQuery` its `Unknown URL` exception would
classify as. A provider that dies mid-write is a `RemoteException` and so
also `unavailable`, where `ContentResolver` would silently have
returned `null` (insert) or `-1` (update, delete).

`openBinary()` reports a provider that returns no stream with native
details `reason: noStream`, which the mapped error keeps under
`details['details']`.

Each mapped error carries:

- the request's domain and operation where known;
- a `simple_query:` message (the host's own, or one naming the call
  target and host code);
- `details`: `code` (the host code), `details` (the host's details,
  including `nativeStackTrace` for a classified exception), plus
  `contentUri` and `nativeType` (the Java class name) when known. A
  failed join query names its own provider: `contentUri` is the join's
  URI, and the host details name the `join`.

The original stack trace is kept.

A null cursor maps to `unavailable` on the `query()` and
`android.provider` `queryWithJoins` paths, never to an empty result. Its
`details.contentUri` is the failing provider's: a related query's for a
join. See
[API_SEMANTICS.md](API_SEMANTICS.md#null-cursor-semantics-android).

### Windows host deadlines

Windows filesystem and store host calls have a 30-second deadline starting
when queued. If an unfinished call reaches its deadline, the host reports
`unavailable` for every unfinished call and refuses further background work
until the plugin is recreated. Replies remain on the platform thread.

The deadline cancels supported store reads. Windows filesystem I/O cannot
always be interrupted: an operation already running may still change files
after its timeout reply. A timeout does not roll back a mutation. Check its
effect before retrying, then restart the app. Queued operations that never
started are dropped; late replies are suppressed.

Windows file mutations validate filters and contents before changing targets.
File inserts never replace existing destinations. Content updates follow symbolic
links and stage replacement bytes before publication, preserving existing file
metadata through Windows replacement semantics. Filesystem mutation failures
include `affectedCount` for confirmed effects and `partialEffectsPossible`;
timeouts or unexpected deferred failures can leave the count unknown.
A `recoveryPath` identifies a retained recovery directory after replacement or
backup-cleanup failure. These errors do not promise rollback of earlier effects.
