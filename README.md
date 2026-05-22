# simple_query

> **Contributing / agents:** see [`AGENTS.md`](AGENTS.md) for build·test·verify (incl. `melos bootstrap`), conventions, and the "What NOT to do" rulings (Claude Code reads it via [`CLAUDE.md`](CLAUDE.md)). Governed by the Simple Zen SOP family (Notion).

Federated Flutter plugin providing a typed query API for device data (contacts,
media, files, calendar, messages, calls). Android is the reference backend;
non-Android platforms provide a mix of native bridges, filesystem fallback, and
honest capability reporting via `getCapabilities()`.

## Repository Layout

Federated-only source of truth:

- `packages/` contains plugin implementations and platform interface.
- `example/` contains sample app and integration tests.

## Packages

- `packages/simple_query`: app-facing facade
- `packages/simple_query_platform_interface`: contracts, models, and errors
- `packages/simple_query_shared`: shared infrastructure for non-Android platforms
- `packages/simple_query_android`: Android backend
- `packages/simple_query_ios`: iOS backend
- `packages/simple_query_macos`: macOS backend
- `packages/simple_query_windows`: Windows backend
- `packages/simple_query_linux`: Linux backend

## Highlights

- Typed filter/query model (`QueryRequest`, `QueryBuilder`) with canonical
  field vocabulary enforced per domain (`QueryFieldCatalog`).
- Capability discovery (`getCapabilities`).
- Deterministic standardized errors (`SimpleQueryError` with machine-readable
  `SimpleQueryErrorCode`).
- Permission-aware: simple_query checks current grant state and fails fast
  with `permissionDenied`, but never requests permissions — that stays the
  app's job (use any permission library you like). See
  [`docs/DESIGN.md`](docs/DESIGN.md#p-ext-simple_query-checks-permissions-but-never-depends-on-simple_permissions).
- Streaming pagination (`SimpleQuery.queryPaginated`) that walks
  `nextCursor` / `nextOffset` to exhaustion, cancellable via the
  subscription.
- Self-closing binary-content wrapper (`BinaryContent`,
  `withBinaryContent`) so callers never juggle bare `handleId` strings.
- Typed record views (`ContactRecord`, `CallRecord`, etc.) with
  `record.raw` (untouched source map) and `record.extras` (OEM/niche
  columns the canonical contract doesn't name).
- First-class custom content provider access via
  `SimpleQuery.queryRaw(contentUri: ...)` for in-house providers.
- Unified observe contract with fallback polling where native streams are
  unavailable.
- Explicit sequential best-effort fallback batch semantics outside Android.
- Explicit `platformData.rootPath` requirement for non-Android fallback
  `files`/`media` operations.

## Platform Support

Capability behavior is explicitly platform- and domain-gated. Check capabilities at runtime before issuing operations.

| Domain | Android | iOS | macOS | Windows | Linux |
| --- | --- | --- | --- | --- | --- |
| contacts | Full CRUD + observe | Native (read-only) | Native (read-only) | WinRT (read-only) | EDS (if installed) |
| media | Full CRUD + stream + observe | Filesystem fallback | Filesystem fallback | Filesystem fallback | Filesystem fallback |
| files | Full CRUD + stream + observe | Filesystem fallback | Filesystem fallback | Filesystem fallback | Filesystem fallback |
| calendar | Full CRUD + observe | Native (read-only) | Native (read-only) | WinRT (read-only) | EDS (if installed) |
| messages | Full CRUD + observe | Not available (OS restricted) | Not available (OS restricted) | Not available (OS restricted) | Not available (OS restricted) |
| calls | Full CRUD + observe | Not available (OS restricted) | Not available (OS restricted) | Not available (OS restricted) | Not available (OS restricted) |
| platformSpecific | Provider extensions | Diagnostic + photos | Diagnostic + photos | Diagnostic + storage | Diagnostic + tracker/XDG |

## Documentation

| Topic | File |
| --- | --- |
| **Design principles** (read before contributing) | [docs/DESIGN.md](docs/DESIGN.md) |
| Cross-platform semantics | [docs/API_SEMANTICS.md](docs/API_SEMANTICS.md) |
| Canonical error codes | [docs/ERROR_MAPPING.md](docs/ERROR_MAPPING.md) |
| Migration from raw ContentProvider / native APIs | [docs/MIGRATION.md](docs/MIGRATION.md) |
| Platform support matrix | [docs/PLATFORM_SUPPORT_MATRIX.md](docs/PLATFORM_SUPPORT_MATRIX.md) |
| Platform extensions (index) | [docs/EXTENSIONS.md](docs/EXTENSIONS.md) |
| Release process | [docs/RELEASE.md](docs/RELEASE.md) |

## Development

```bash
# Per-package tests
cd packages/simple_query
flutter test

# Whole-workspace verification
dart run melos run analyze
dart run melos run test

# Regenerate pigeon glue after editing any pigeon.dart
./tool/regen_pigeon.sh
```
