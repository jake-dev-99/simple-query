# Platform Support Matrix

This matrix reflects the current implementation state.

## Domains by Platform

| Domain | Android | iOS | macOS | Windows | Linux |
| --- | --- | --- | --- | --- | --- |
| contacts | read/write/observe | read/observe (CNContactStore) | read/observe (CNContactStore) | read/observe (WinRT) | read/observe (EDS, if installed) |
| media | read/write/observe/stream | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) |
| files | read/write/observe/stream | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) | read/write/observe/stream (filesystem fallback) |
| calendar | read/write/observe | read/observe (EventKit) | read/observe (EventKit) | read/observe (WinRT) | read/observe (EDS, if installed) |
| messages | read/write/observe/stream | not available (OS restricted) | not available (OS restricted) | not available (OS restricted) | not available (OS restricted) |
| calls | read/write/observe | not available (OS restricted) | not available (OS restricted) | not available (OS restricted) | not available (OS restricted) |
| platformSpecific | provider extension namespace | read-only diagnostic/photos | read-only diagnostic/photos | read-only diagnostic/storage | read-only diagnostic/tracker/XDG |

## Notes

- `getCapabilities()` is the source of truth at runtime and must gate feature usage.
- Unsupported operations return `SimpleQueryError` with `SimpleQueryErrorCode.notSupported`.
- Non-Android platforms provide filesystem fallback for `files` and `media`, including polling-based observe and binary handle lifecycle.
- Contacts and calendar are read-only on all non-Android platforms. Write support requires additional native implementation.
- Linux contacts/calendar require `libebook-1.2` and `libecal-2.0` (EDS client libraries). When unavailable, queries return EDS source metadata as a fallback.
- Extension methods may return diagnostic/synthetic responses marked with an `implementation` field in the payload.
- Batch semantics are ordered on every platform. Outside Android, batch execution is `sequentialBestEffort` and surfaces per-operation failures in result metadata.
- Default root paths are provided per-platform for files/media fallback queries. Callers can override with `platformData.rootPath`.
