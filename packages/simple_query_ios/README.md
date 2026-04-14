# simple_query_ios

iOS platform backend for `simple_query`.

Current implementation:

- `files` and `media` domains: local filesystem query/mutate/observe/binary support
- Restricted domains (`messages`, `calls`) return deterministic `SimpleQueryErrorCode.notSupported`
- `contacts` and `calendar` are read-only via native backend
- `ios.contacts` and `ios.calendar` extension methods provide diagnostic metadata
- `ios.photos` uses filesystem fallback and labels those responses accordingly
