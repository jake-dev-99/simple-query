# simple_query_windows

Windows platform backend for `simple_query`.

Current implementation:

- `files` and `media` domains: local filesystem query/mutate/observe/binary support
- Restricted domains (`messages`, `calls`) return deterministic `SimpleQueryErrorCode.notSupported`
- `contacts` and `calendar` are read-only (contacts/calendar via native backend)
- `windows.storage`, `windows.contacts`, and `windows.calendar` extension methods provide diagnostic metadata
- Batch fallback semantics are ordered and `sequentialBestEffort`, not atomic
