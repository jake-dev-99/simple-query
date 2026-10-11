# API Semantics

This document defines the portability guarantees for the simple_query API.

## Query fields

- `entityType`: stable core field. Platforms may ignore it when a domain does not expose multiple entity types.
- `projection`: stable core field. Backends should return only requested keys when projection is supported.
- `include`: not part of the `QueryRequest` model. For join-style queries on Android, use `callExtension(namespace: 'android.provider', method: 'queryWithJoins')`. The canonical model deliberately avoids SQL-join semantics — most domains don't support them and the ones that do (Android contacts) are better served by the typed extension.
- `platformData`: platform-specific escape hatch. Treat keys as namespace-specific and non-portable unless documented otherwise.

## Batch semantics

- Result ordering is stable and always matches the input operation order.
- Android may execute provider batches atomically when the underlying provider supports it.
- Non-Android fallback backends execute batches with `sequentialBestEffort` semantics.
- Non-Android fallback backends require `platformData.rootPath` for `files` and `media` operations, including batch execution.
- In sequential best-effort mode, later operations still run after an earlier failure.
- Sequential best-effort failures are surfaced in `MutationResult.metadata.error`.

## Extension semantics

- `callExtension()` is the entry point for platform-specific operations that are not portable enough for the core API.
- Extension payloads may include `implementation` metadata.
- `implementation = filesystem_fallback` means the result came from local filesystem-derived behavior.
- `implementation = diagnostic_synthetic` means the result is a diagnostic or placeholder response, not authoritative provider data.

## Null cursor semantics (Android)

On Android, `ContentResolver.query(...)` returns a null cursor when the provider could not be reached (unknown authority, or its process died during the call) or chose to return none. That is not the same as zero matching rows, and the cause may be transient.

- `query()` (and everything built on it: `queryRaw`, `queryTyped`, `queryBuilder`, `queryPaginated`, `queryPaginatedTyped`) throws `SimpleQueryError(code: unavailable)` for a null cursor. The error carries the request's `domain`, `operation: read`, a `simple_query:` message, and `details` with the failing provider's `contentUri` plus the native code and details (`reason: nullCursor`). An empty `records` list always means the query ran and matched zero rows.
- `callExtension(namespace: 'android.provider', method: 'queryWithJoins')` throws the same error when the primary query or any related (join) query gets a null cursor; `domain` is `platformSpecific`; when a related query failed, `details.contentUri` is that join's URI and the native details name the `join`. A related query that runs and matches nothing still yields an empty list for that join.
- Before `simple_query_android` 0.3.0 both paths returned an empty result for a null cursor. Callers that want the old behavior for an optional provider catch `SimpleQueryErrorCode.unavailable` and substitute an empty result themselves.
- A permission denial is not a null cursor. The Dart pre-flight check still throws `SimpleQueryError(permissionDenied)` before the provider is called. A `SecurityException` raised by the provider itself also maps to `permissionDenied`, but without `details.permissions`; see [Host error semantics](#host-error-semantics-android).

## Host error semantics (Android)

Since `simple_query_android` 0.3.0, no Android host call lets a raw `PlatformException` reach callers. Every host failure becomes a `SimpleQueryError`, with the code chosen by the table in [ERROR_MAPPING.md](ERROR_MAPPING.md#android-host-errors). Before 0.3.0, host errors arrived as a raw `PlatformException`, so `on SimpleQueryError` handlers missed them.

- The permission pre-flight `SimpleQueryError(permissionDenied)` carries the request's `domain` and operation, and its `details.permissions` name the OS permissions to request. A provider-raised denial has no such list; its `details.nativeType` names the exception (e.g. `java.lang.SecurityException`).
- `observe()`: a failed observer registration is delivered to listeners as a stream error. A failed unregistration has no listener left to receive it, so its mapped error goes to the zone's uncaught-error handler. That covers a cancel, and a registration that completes after its only listener cancelled.
- `openBinary()` maps failures of the host call, which sets up the pipe. The provider content is opened and copied afterwards, in the background; a failure there arrives as an `ObserveEvent` with `source: android.stream_error` and ends the pipe early, not as a `SimpleQueryError`.
- The `android.provider` `extractToFile` extension raises the mapped failure of opening or copying the content (e.g. `permissionDenied` for a provider `SecurityException`). It raises `unavailable` only when the provider hands back no stream.
- `dispose()` runs every cleanup step (observer unregistration, binary stream close, event stream close) even when a host call fails, then throws the first failure.
- `unavailable` means only that the provider or host could not be reached. A failure no rule covers is `transientFailure`, so code that tolerates `unavailable` (for example, an optional provider) does not hide unrelated bugs.

## Native Kotlin helper semantics (Android)

`ContentQuery` (in `simple_query_android`) is the in-process Kotlin API sibling plugins use for `content://` reads. It is not part of the Dart platform interface.

- `ContentQuery.queryOrNull(...)` returns `null` if and only if `ContentResolver.query(...)` returned a null cursor: the provider could not be reached (unknown authority, or its process died during the call) or chose to return none. The cause may be transient, so callers must not treat `null` as a permanent state. Otherwise it returns the drained rows; an empty list means the query ran and matched zero rows. This `null` is the in-process counterpart of the Dart path's `SimpleQueryError(unavailable)` (see [Null cursor semantics](#null-cursor-semantics-android)).
- `ContentQuery.query(...)` delegates to `queryOrNull(...)` and returns an empty list for a null cursor, so a null cursor and zero matching rows are indistinguishable. Callers that must tell them apart use `queryOrNull(...)`.
- Neither method catches resolver exceptions; they propagate to the caller. A permission denial surfaces this way (`SecurityException`), not as a null cursor.

## Pagination semantics

`QueryPage` supports two modes:

- **Offset-based:** Set `limit` and `offset`. The response includes `nextOffset` (null when no more pages). Simple but can skip or duplicate records if the underlying data changes between pages.
- **Cursor-based:** Set `limit` and `cursor` (an opaque string from a previous `QueryResult.nextCursor`). When `cursor` is set, `offset` is ignored. The response includes `nextCursor` (null when no more pages). More reliable for datasets that change frequently (e.g., messages, call logs).

Platform implementations encode cursors differently:

- Android: last-seen `_id` value, used as `WHERE _id > ?`
- Filesystem fallback: last-seen record `id` (file path), used to skip past already-seen records
- Native non-Android bridges: cursor passed through to native code and decoded on the platform side

## Capability semantics

- `getCapabilities()` is the source of truth for whether a domain/operation is currently usable.
- Documentation must not claim support beyond what runtime capabilities advertise.
- Unsupported domains should return `SimpleQueryErrorCode.notSupported` rather than synthetic success payloads.
- Backends that return incomplete capability snapshots or records outside the documented core schema fail closed with deterministic `SimpleQueryError`.
