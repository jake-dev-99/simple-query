## Unreleased

## 0.6.0

Documentation and developer-experience release. Strictly additive — no public-API changes from 0.5.0.

### Added
- `docs/DESIGN.md` — codifies the seven design principles (P1–P7 plus the permissions rule) that govern the public API. Read before contributing.
- `docs/EXTENSIONS.md` — single index linking the five per-platform extension docs with a usage example and stability policy.
- `tool/regen_pigeon.sh` already landed in 0.5.0; documented here for completeness.
- Every package's `CHANGELOG.md` now leads with a `## Unreleased` section. `tool/publish.sh` warns when the section is still present during a live publish.

### Changed
- Root `README.md` now links every doc (DESIGN, API_SEMANTICS, ERROR_MAPPING, MIGRATION, PLATFORM_SUPPORT_MATRIX, EXTENSIONS, RELEASE) in a navigation table. "Highlights" expanded to cover the 0.4.0 / 0.5.0 additions.
- `docs/MIGRATION.md` rewritten to cover iOS / macOS / desktop migrations alongside Android. Includes the column-name → canonical-field translation table for 0.4.0's symmetric vocabulary.
- `docs/API_SEMANTICS.md` — "`include` reserved for future use" replaced with a clear non-goal statement and pointer to `callExtension('android.provider', 'queryWithJoins')`.
- `docs/RELEASE.md` — adds a "CHANGELOG discipline" section documenting the Unreleased → release promotion flow.
- `SimpleQueryPlatform` (platform interface) now has class-level and per-method dartdoc. Documents that `instance =` is not thread-safe.
- `SimpleQuery` (facade) class doc calls out the singleton contract explicitly and explains how tests swap the backend (via `SimpleQueryPlatform.instance`, not the facade).
- Every facade passthrough (`query`, `mutate`, `batch`, `observe`, `callExtension`, `dispose`) now has a dartdoc block with cross-links.
- `QueryDomainContracts` and `CapabilityContracts` have class-level dartdoc.

### Example app
- Rewritten to showcase the full 0.5.0 API: `queryBuilder`, `queryPaginated`, `mutate` round-trip, `observe` with live filesystem events, `withBinaryContent`, `callExtension` (platform-aware), and `queryRaw` (Android). Buttons are capability-gated; a refresh button re-probes.

### Migration
None required — docs/example/dartdoc changes only.

## 0.5.0

Consumer-ergonomics release. Additive — no breaking changes from 0.4.0.

### Added
- `SimpleQuery.queryPaginated(request)` — returns a `Stream<QueryResult>` that walks `nextCursor` / `nextOffset` to exhaustion. Cursor is preferred when both are present; stops on the first empty page or when both pagination tokens are null. Cancellable via the stream subscription.
- `SimpleQuery.queryPaginatedTyped<T>(request, fromRecord)` — same but maps each page through `fromRecord`. Mapping failures wrapped in `SimpleQueryError(invalidQuery, details: {recordIndex, cause})` like `queryTyped`.
- `BinaryContent` class wraps `BinaryContentHandle` with typed getters and a self-closing `close()` that's idempotent.
- `SimpleQuery.openBinaryContent(request)` — opens binary content as a `BinaryContent`.
- `SimpleQuery.withBinaryContent(request, body)` — scoped helper that runs `body` with an open `BinaryContent` and guarantees close on return or throw.
- `SimpleQuery.queryBuilder(domain)` — fluent entry point for building queries off the facade instead of importing `QueryBuilder` directly.
- `QueryBuilder.pageOffset({limit, required offset})` and `pageCursor({limit, required cursor})` — type-safe pagination convenience methods.
- `QueryBuilder.executePaginated()` — convenience for `queryPaginated(build())`.

### Changed
- `QueryBuilder.build()` now validates filter/sort/projection field names against `QueryFieldCatalog` before returning. Unknown canonical fields throw `SimpleQueryError(invalidQuery, details: {field, allowed})` instead of failing later in the platform layer with a less actionable error. No-op for `QueryDomain.platformSpecific`.

### Tooling
- Pigeon dependency pinned to exact version `22.7.4` in every platform package — different developers will produce identical generated output.
- New `tool/regen_pigeon.sh` regenerates every platform's pigeon glue and runs `dart format` on the output for stable line-wrap.
- New `pigeon-drift` CI job re-runs the regen tool and fails the build if the committed `.g.dart` / `.g.kt` are stale. Catches "edited pigeon.dart but forgot to commit the regen" before merge.

### Migration
None required. All additions are new methods; existing code keeps working.

### Deferred
- Typed `callExtension` registry (plan 2.5) — needs concrete consumer demand to design correctly; raw `callExtension` remains the supported path.

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

### Coordinated release
- Part of the lock-step 0.3.1 release across all `simple_query_*` packages. See the matching entry in `simple_query` and `simple_query_platform_interface` for the cross-package work (strict record type coercion, extended runtime validation, `copyWith` null-clearing, constructor guards on `QueryPage` / `QueryFilterCondition`, shared `FakeSimpleQueryPlatform`).

### Fixed
- `SimpleQueryAndroid.registerWith()` no longer throws
  `"Binding has not yet been initialized"` when called from Flutter's
  generated Dart plugin registrant. `SimpleQueryAndroidApi`'s
  constructor calls `QueryFlutterApi.setUp(this)`, which touches the
  default binary messenger — but the registrant runs before
  `WidgetsFlutterBinding.ensureInitialized()`, so eager construction
  failed, the registrant's `try/catch` swallowed the error, and
  `SimpleQueryPlatform.instance` silently stayed as the
  "unsupported" stub. Every `query` / `observe` call on Android then
  threw `notSupported` against consumer apps. `_api` is now
  `late final`, deferring construction to first method call — by
  which point `runApp` has run and the binding is ready.

## 0.2.1

- Bump `simple_permissions_native` dependency constraint from `^0.0.1` to `^1.2.0` to match the current published version. No API changes.

## 0.2.0

- Initial public release.
- Typed query API with 7 domains: contacts, media, files, calendar, messages, calls, platformSpecific.
- Cursor-based and offset-based pagination.
- Capability-driven design via `getCapabilities()`.
- Typed record classes: `ContactRecord`, `CalendarEventRecord`, `MediaRecord`, `FileRecord`, `MessageRecord`, `CallRecord`.
- Structured error model with 5 error codes.
