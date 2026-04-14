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
