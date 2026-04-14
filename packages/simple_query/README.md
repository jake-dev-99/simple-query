# simple_query

Cross-platform query facade with typed filters, capability discovery, unified observe events, and binary content handles.

## Core API

- `getCapabilities()`
- `query(QueryRequest)`
- `mutate(MutationRequest)`
- `batch(BatchRequest)`
- `observe(ObserveRequest)`
- `openBinary(BinaryRequest)` / `closeBinary(handleId)`
- `callExtension(namespace, method, args)`

## Example

```dart
await SimpleQuery.initialize();

final capabilities = await SimpleQuery.instance.getCapabilities();

final result = await SimpleQuery.instance.query(
  const QueryRequest(
    domain: QueryDomain.contacts,
    page: QueryPage(limit: 50, offset: 0),
  ),
);
```

Use `getCapabilities()` before domain operations and handle `SimpleQueryError` with `SimpleQueryErrorCode.notSupported` for restricted domains.
