## 0.4.0

### Breaking changes
- **Filter / sort / projection field vocabulary is now canonical** on every named domain. Callers must pass the canonical names (`callType`, `timestamp`, `durationSec`, `id`, ...) defined in `QueryFieldCatalog.canonicalFields(domain)`. Raw native column names (`type`, `date`, `_id`, ...) on a named domain throw `SimpleQueryError(invalidQuery, details: {field, allowed})`. Use `QueryDomain.platformSpecific` (or `SimpleQuery.queryRaw`) when you need raw column names.
- **Android: `simple_permissions_native` dependency dropped.** The `SimpleQueryAndroidApi` constructor no longer takes a `checkPermission` callback. simple_query checks permissions inline via a new pigeon `hasPermission` call and throws `SimpleQueryError(permissionDenied, details: {permissions: [...], contentUri, write})` when a permission is missing. Callers request the permission via their own mechanism (`permission_handler`, `simple_permissions`, raw `ActivityCompat.requestPermissions`) and retry.

### Added
- `QueryFieldCatalog` (in `simple_query_platform_interface`) — single source of truth for the canonical field vocabulary per domain. Exposes `canonicalFields(domain)`, `ensureKnown({domain, canonical})`, and `ensureAllKnown({domain, fields})`. No-op for `QueryDomain.platformSpecific`.
- `RuntimeContractValidation.validateQueryRequest` / `validateMutationRequest` (extended) / `validateObserveRequest` (extended) now reject unknown canonical fields with a clear error before reaching the platform.
- `SimpleQuery.queryRaw({contentUri, ...})` — first-class entry point for arbitrary content providers (`content://com.biz.app/...`). Bypasses canonical validation; returns raw `Map<String, Object?>` records. Currently Android-only.
- Every typed `*Record` (Contact, CalendarEvent, Media, File, Message, Call) now exposes `raw: Map<String, Object?>` (untouched source map) and `extras: Map<String, Object?>` (raw minus canonical keys). Surfaces OEM and niche columns without losing the typed view.
- `CallRecord` gains the optional canonical fields `isNew`, `isRead`, `geocodedLocation`, `subscriptionId`, populated by Android from native columns `new`, `is_read`, `geocoded_location`, `subscription_id`. Absent (not defaulted) on devices that don't expose the column. Closes the gap surfaced by `simple-telephony`'s `listCallLog` handoff.

### Changed
- Android backend now translates canonical filter / sort / projection field names to native ContentResolver columns symmetrically (output records were already canonical). New `_AndroidFieldAliases` helper centralises the mapping; `platformSpecific` skips translation.
- Apple `permissionDenied` errors now carry `details: {framework, infoPlistKey, authorizationStatus}` so callers learn exactly which Info.plist usage description to declare.
- Filesystem fallback (Linux / Windows / non-Android desktop) maps `FileSystemException` with errno 1 / 13 / 5 (EPERM, EACCES, ERROR_ACCESS_DENIED) to `SimpleQueryErrorCode.permissionDenied` instead of the previous `unavailable`.
- `validateBatchRequest` no longer cascades to per-operation validation. Batches are sequentialBestEffort — a malformed op records its error in per-result `metadata.error` instead of aborting the batch.

### Migration
- If you currently filter or sort on a named domain using raw Android column names, switch to canonical names. Example: `field: 'type'` → `field: 'callType'`, `field: 'date'` → `field: 'timestamp'`, `field: '_id'` → `field: 'id'`. Full catalog: `QueryFieldCatalog.canonicalFields(domain)`.
- If you constructed `SimpleQueryAndroidApi` directly with `checkPermission:`, drop the argument. The internal pigeon-based permission check replaces it. Permission requests stay your responsibility.
- If your app handled `SimpleQueryErrorCode.unavailable` for filesystem permission errors, add a `permissionDenied` branch.

## 0.3.1

### Added
- Strict type coercion in record parsers (`records.dart`). `ContactRecord`, `CalendarEventRecord`, `MediaRecord`, `FileRecord`, `MessageRecord`, `CallRecord` now throw `SimpleQueryError(invalidQuery)` with the offending field name and runtimeType instead of silently returning `[]`, `"Instance of 'Map'"`, or misinterpreting `1`/`"true"` as `false`.
- Runtime contract coverage: `RuntimeContractValidation` gains `validateMutationRequest`, `validateBatchRequest`, `validateObserveRequest`, `validateBinaryRequest`. `validateQueryResult` now includes `domain` in error details.
- Constructor guards: `QueryPage` asserts `offset` and `cursor` are mutually exclusive and adds `QueryPage.offset` / `QueryPage.cursor` named constructors. `QueryFilterCondition` asserts that `inList` operator carries a `List` value.
- New `testing.dart` entry point exports `FakeSimpleQueryPlatform` for use by both this package's own tests and sibling packages, replacing the duplicated private fakes that had drifted between them.

### Changed
- `SimpleQueryError.toString()` now appends the `details` map so log output surfaces the contextual fields (`missingDomains`, `recordIndex`, `field`, etc.) that were being constructed but never rendered.
- Every `copyWith` that takes nullable fields now accepts explicit `null` to clear the value. Previously all `copyWith` methods used `field ?? this.field`, making it impossible to move a `QueryPage` from offset-mode to cursor-mode or to unset `metadata` / `nextOffset` / `nextCursor` / `entityType` / `pollingInterval` on any request/result. Uses an `_unset` sentinel under the hood.

### Developer-facing
- Coordinated 0.3.1 release across all simple_query packages. Adopting lock-step versioning so consumers see a single matching version across facade, interface, and platform implementations.

## 0.2.0

- Initial public release.
- Typed query API with 7 domains: contacts, media, files, calendar, messages, calls, platformSpecific.
- Cursor-based and offset-based pagination.
- Capability-driven design via `getCapabilities()`.
- Typed record classes: `ContactRecord`, `CalendarEventRecord`, `MediaRecord`, `FileRecord`, `MessageRecord`, `CallRecord`.
- Structured error model with 5 error codes.
