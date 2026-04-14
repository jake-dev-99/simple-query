# macOS Extensions

- Namespace `macos.contacts`
  - `listSources`
    - required args: none
    - optional args: none
    - response shape: `{ sources: List<Map<String,Object?>>, authorizationStatus: String }`
  - `listGroups`
    - required args: none
    - optional args: none
    - response shape: `{ groups: List<Map<String,Object?>>, authorizationStatus: String }`
- Namespace `macos.calendar`
  - `listCalendars`
    - required args: none
    - optional args: none
    - response shape: `{ calendars: List<Map<String,Object?>>, authorizationStatus: String }`
  - `getDefaultTimeZone`
    - required args: none
    - optional args: none
    - response shape: `{ timeZone: String }`
- Namespace `macos.photos`
  - `fetchAssetResources`
    - required args: none
    - optional args: `rootPath`, `limit`
    - response shape: `{ resources: List<Map<String,Object?>> }`
  - `listMediaTypes`
    - required args: none
    - optional args: none
    - response shape: `{ mediaTypes: List<String> }`


Note: Some extension responses include an `implementation` field. `diagnostic_synthetic` means the response is metadata, not live data from the native backend. `filesystem_fallback` means the data came from local filesystem enumeration. See `docs/API_SEMANTICS.md` for details.

Error mapping:
- invalid args -> `invalidQuery`
- unknown method/namespace -> `notSupported`
- native framework unavailable -> `unavailable`
