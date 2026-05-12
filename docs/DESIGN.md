# Design Principles

This document codifies the design decisions that govern the simple_query
public API. Read it before proposing changes to the core contract,
adding a new domain, or integrating simple_query into another plugin.

These are **decisions, not preferences.** Each one is load-bearing for the
federated architecture; changing one means revisiting the rest.

---

## P1. Pigeon carries transport primitives, not domain types

The pigeon-generated host APIs in each platform package
(`QueryRequest`, `QueryResponse`, `InsertRequest`, etc.) are
**transport types**. They carry primitives (`String` URIs, `List<String>`
projections, `Map<Object?, Object?>` rows) across the platform channel and
nothing more.

The **domain types** the public API exposes — `QueryDomain`, `QueryRequest`,
`QueryResult`, `CapabilitySnapshot`, `ContactRecord`, `CallRecord`, etc. —
live in `packages/simple_query_platform_interface/lib/src/models.dart` and
`records.dart`. They are hand-written, not generated.

**Why:** pigeon's strength is wire serialisation. Consumer-facing types
need versioning, copyWith semantics, equality, dartdoc, and evolution
independent of the wire format. Coupling them would freeze the public API
to pigeon's expressive limits and bloat every regeneration.

**Consequence:** adding a new domain never touches `pigeon.dart`. It
touches `contracts.dart`, `records.dart`, and each platform's
`_normalizeRecord` implementation.

---

## P2. One canonical record type per domain — never per-OS-version

Android's `CallLog.Calls` has accumulated columns monotonically since API 1.
The core fields (`number`, `date`, `duration`, `type`) have been stable
the whole time. OEMs add more quarterly. Versioning types per Android SDK
(`CallsV29Record` / `CallsV33Record`) is a trap:

- Ships stale the day after any OEM release (Samsung's `is_business_call`,
  `sec_preferred_sim`, etc. change without an AOSP bump).
- Explodes the type count: 6 domains × ~8 SDKs × 4 platforms = ~200 classes.
- Forces the app developer to branch on `Build.VERSION.SDK_INT` from Dart
  just to pick the right `fromRecord` constructor.

**Rule:** one canonical `*Record` class per domain. OS and OEM variance
is expressed via **optional fields** in the contract, not subtype
proliferation. A consumer learns one schema, forever.

---

## P3. Baseline the canonical schema on AOSP, not a real device

Evidence: a Samsung One UI device emits ~50 columns per `call_log.calls`
row; AOSP's `CallLog.Calls` has ~25. Samsung adds `sec_*`, `phone_account_*`,
`composer_photo_uri`, `call_screening_*`, `transcription_*`, `missed_reason`,
`is_business_call`. Same pattern in `contacts_contacts` (`sec_custom_vibration`,
`sec_preferred_video_call_account_name`, ...) and `sms`
(`teleservice_id`, `spam_report`, `bin_info`, ...).

**Rule:** the canonical schema (`QueryDomainContracts.requiredKeys` /
`optionalKeys`, `QueryFieldCatalog.canonicalFields`) mirrors **AOSP**, not
any one OEM or SDK level. OEM extensions surface via `record.raw` and
`record.extras` (P4), not the canonical contract.

**Why:** baselining on Samsung would leave every Pixel / Nothing /
AOSP-ROM user missing ~30 fields per domain; the inverse is graceful —
Samsung users still get the canonical set plus a populated `extras` map.

---

## P4. Three-axis field access per typed record

Every `*Record` (Contact, CalendarEvent, Media, File, Message, Call)
exposes:

1. **Typed getters** — canonical contract fields (`contact.displayName`,
   `call.callType`, `call.durationSec`). Type-safe, IDE-autocompletes,
   documented contract. What 90% of callers use.
2. **`record.raw: Map<String, Object?>`** — the untouched source map
   exactly as the platform returned it. Full fidelity, zero interpretation.
   For consumers that need OEM/niche columns the canonical contract
   doesn't promise.
3. **`record.extras: Map<String, Object?>`** — computed at construction:
   `raw` minus every key the canonical contract for this domain already
   covers. Surfaces OEM and platform-specific extensions without
   duplicating fields already exposed via typed getters.

Not either/or. Consumers pick per call site. Both `raw` and `extras` are
unmodifiable maps.

---

## P5. First-class custom content providers (Android)

Apps that host their own `com.biz.app.provider` should be able to hit it
through simple_query — not as an escape hatch, as a supported entry point.

```dart
final result = await SimpleQuery.instance.queryRaw(
  contentUri: 'content://com.biz.app/data',
  filters: [...], sort: [...], page: QueryPage(limit: 50),
);
// result.records are raw Map<String, Object?> — no canonical validation.
```

Mechanics:

- Same pigeon pipeline as a named-domain query.
- Bypasses canonical field translation, `_normalizeRecord`, and
  `RuntimeContractValidation.validateQueryResult`.
- Returns raw rows with native column names exactly as the provider
  emitted them.
- Permission resolution still runs: the authority maps to an OS
  permission string via the inline catalog (see P-ext below), so a missing
  grant throws `SimpleQueryError(permissionDenied, details: {permissions})`
  just like a named-domain query.

Named domains remain the opinionated, typed, canonical path. `queryRaw`
is the **explicitly opt-in** untyped path.

---

## P6. Prospector as a runtime capability oracle (future plugin)

Reserved for a later release. Prospector is a sibling Flutter plugin that
introspects the device at runtime:

```dart
final catalog = await Prospector.instance.describe('com.android.calllog');
// → ProviderCatalog { authority, columns: List<ColumnDescriptor>, sampleRow? }
```

- **Does not replace typed records.** Consumers that want static typing
  stay on `SimpleQuery.instance.query(QueryDomain.calls)`.
- **Supplies runtime truth about this device.** Apps that need to branch
  on "does this device surface `geocoded_location`?" can ask Prospector
  instead of guessing from `Build.MANUFACTURER`.
- **Target release:** 0.7.0+ once the design is stable.

---

## P7. `quicktype_dart` is deferred, not rejected

Hand-written `records.dart` (~250 lines for 6 domains) is fine at the
current scale. If/when the project commits to an AOSP JSON schema
baseline (per P3), `quicktype_dart` becomes the natural tool to generate
`records.dart` + `contracts.dart` from that schema.

**Decision gate:** adopt `quicktype_dart` only when both

1. An AOSP JSON schema snapshot per domain lands at
   `packages/simple_query_platform_interface/schemas/`, and
2. `tool/` has a documented regeneration step.

Until then, hand-written types stay. One codegen pipeline (pigeon for the
wire) is enough.

---

## P-ext. simple_query checks permissions but never depends on `simple_permissions`

Permissions are data-access's constant companion, and simple_query is
opinionated about them:

**What simple_query does:**

- **Checks** current grant state inline before hitting the native data
  API. Android: pigeon `hasPermission(name)` wrapping
  `Context.checkSelfPermission`. iOS / macOS: `CNContactStore` /
  `EKEventStore` `authorizationStatus(for:)`. Desktop: filesystem
  `FileSystemException(EACCES)` mapping.
- **Fails fast** with `SimpleQueryError(code: permissionDenied,
  details: {permissions: [...], contentUri, operation})`. The details
  map names the exact OS permission identifier the caller needs to
  request next.

**What simple_query does not do:**

- **Depend on `simple_permissions`, `permission_handler`, or any other
  request framework.** Past guidance was the opposite; the rule is
  inverted intentionally.
- **Request permissions.** No dialogs, no intents, no async waiting on
  grant. The developer chooses how to request — directly via
  `ActivityCompat.requestPermissions`, via their preferred permissions
  package, doesn't matter — and retries the simple_query call after the
  grant succeeds.

**Why:** the permission catalog (what domain needs what OS permission) is
stable data-access knowledge that belongs to simple_query. The request
flow — UX, retry orchestration, rationale dialogs — belongs to the app.
Conflating them fragments permission UX across consumers.

---

## Extension points

| What | Where | Why |
| --- | --- | --- |
| Add a new domain | `QueryDomain` enum + `QueryDomainContracts` + per-platform `_normalizeRecord` + optional `*Record` typed view | Contract-first |
| Add a canonical field | `QueryDomainContracts.optionalKeys` + per-platform aliases + `*Record` field + migration note | Additive, never per-SDK |
| Add a platform-specific feature | `callExtension(namespace: '<platform>.<area>', method: ...)` + a section in `docs/extensions/<platform>.md` | Non-portable stays non-portable |
| Query a custom content provider | `SimpleQuery.instance.queryRaw(contentUri: ...)` | First-class, not an escape hatch |
| Need permission state | Inline catalog (`AndroidQueryPermissionResolver`, Apple `authorizationStatus` helpers) | No external deps |

---

## Non-goals

These are not things simple_query will ever do:

- **Request permissions on the caller's behalf** (P-ext).
- **Generate typed models per OS/SDK version** (P2).
- **Unify into one Kotlin/Swift/C++ monolith behind a single channel** —
  the federated structure is deliberate.
- **Ship per-field schema validation that transforms records in flight.**
  `RuntimeContractValidation.validateQueryResult` checks the contract
  surface (required keys present) and fails closed; it does not coerce.
