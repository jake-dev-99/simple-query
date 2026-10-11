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

- Android: a null cursor from `ContentResolver.query(...)` (provider
  unreachable, or none returned) maps to `unavailable` on the `query()` and
  `android.provider` `queryWithJoins` paths, never to an empty result. The
  native side raises Pigeon error code `unavailable`; the Dart side converts
  it to `SimpleQueryError(unavailable)` with domain, `operation: read`, a
  `simple_query:` message and `details.contentUri` (the failing provider's;
  a related query's for a join). See
  [API_SEMANTICS.md](API_SEMANTICS.md#null-cursor-semantics-android).
