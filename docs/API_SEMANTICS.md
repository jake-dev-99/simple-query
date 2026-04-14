# API Semantics

This document defines the portability guarantees for the simple_query API.

## Query fields

- `entityType`: stable core field. Platforms may ignore it when a domain does not expose multiple entity types.
- `projection`: stable core field. Backends should return only requested keys when projection is supported.
- `include`: reserved for future use. Not currently part of the `QueryRequest` model. On Android, use `callExtension(namespace: 'android.provider', method: 'queryWithJoins')` for join-style queries.
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
