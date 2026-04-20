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
