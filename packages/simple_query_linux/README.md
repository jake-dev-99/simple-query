# simple_query_linux

Linux platform backend for `simple_query`.

Current implementation:

- `files` and `media` domains: local filesystem query/mutate/observe/binary support
- Restricted domains (`messages`, `calls`) return deterministic `SimpleQueryErrorCode.notSupported`
- `contacts` and `calendar` are read-only (contacts/calendar via native backend)
- `linux.tracker` and `linux.xdg` extension methods provide diagnostic metadata
- Batch fallback semantics are ordered and `sequentialBestEffort`, not atomic
