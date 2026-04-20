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
