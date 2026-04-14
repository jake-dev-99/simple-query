# Windows Extensions

- Namespace `windows.contacts`
  - `listStores`
    - required args: none
    - optional args: none
    - response shape: `{ stores: List<Map<String,Object?>> }`
- Namespace `windows.calendar`
  - `listCalendars`
    - required args: none
    - optional args: none
    - response shape: `{ calendars: List<Map<String,Object?>> }`
- Namespace `windows.storage`
  - `resolveKnownFolders`
    - required args: none
    - optional args: `includeTemp`
    - response shape: `{ folders: List<Map<String,Object?>> }`
  - `listLibraries`
    - required args: none
    - optional args: none
    - response shape: `{ libraries: List<String> }`


Note: Some extension responses include an `implementation` field. `diagnostic_synthetic` means the response is metadata, not live data from the native backend. `filesystem_fallback` means the data came from local filesystem enumeration. See `docs/API_SEMANTICS.md` for details.

Error mapping:
- invalid args -> `invalidQuery`
- unknown method/namespace -> `notSupported`
- platform API unavailable -> `unavailable`
