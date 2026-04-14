# Linux Extensions

- Namespace `linux.eds`
  - `listAddressBooks`
    - required args: none
    - optional args: none
    - response shape: `{ addressBooks: List<Map<String,Object?>> }`
  - `listCalendars`
    - required args: none
    - optional args: none
    - response shape: `{ calendars: List<Map<String,Object?>> }`
- Namespace `linux.tracker`
  - `listIndexScopes`
    - required args: none
    - optional args: `limit`
    - response shape: `{ scopes: List<Map<String,Object?>> }`
  - `listGraphNames`
    - required args: none
    - optional args: none
    - response shape: `{ graphs: List<String> }`
- Namespace `linux.xdg`
  - `listIndexScopes`
    - required args: none
    - optional args: `includeTemp`
    - response shape: `{ scopes: List<Map<String,Object?>> }`


Note: Some extension responses include an `implementation` field. `diagnostic_synthetic` means the response is metadata, not live data from the native backend. `filesystem_fallback` means the data came from local filesystem enumeration. See `docs/API_SEMANTICS.md` for details.

Error mapping:
- invalid args -> `invalidQuery`
- unknown method/namespace -> `notSupported`
- DBus/indexer unavailable -> `unavailable`
