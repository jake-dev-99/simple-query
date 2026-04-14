# Android Extensions

Namespace: `android.provider`

- `providerCall`
  - required args: `authority`, `method`
  - optional args: `arg`, `extras`
  - response shape: `{ ok: bool, method?: String, ...providerPayload }`
  - error mapping: invalid args -> `invalidQuery`, unsupported method -> `notSupported`, provider failure -> `unavailable`/`transientFailure`
- `queryWithJoins`
  - required args: `contentUri`, `joins`
  - optional args: `projection`, `selection`, `selectionArgs`, `sortOrder`, `limit`, `offset`
  - response shape: `{ rowCount: int, rows: [{ data: Map<String,Object?>, related: Map<String,List<Map<String,Object?>>> }] }`
  - error mapping: malformed join payload -> `invalidQuery`, provider failure -> `unavailable`/`transientFailure`
- `extractToFile`
  - required args: `contentUri`
  - optional args: none
  - response shape: `{ contentUri: String, path: String }`
  - error mapping: missing arg -> `invalidQuery`, missing source -> `unavailable`
