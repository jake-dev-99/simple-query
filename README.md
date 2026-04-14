# simple_query

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

- Typed filter/query model (`QueryRequest`)
- Capability discovery (`getCapabilities`)
- Deterministic standardized errors (`SimpleQueryError`)
- Unified observe contract with fallback polling where native streams are unavailable
- Binary content handle abstraction (`openBinary` / `closeBinary`)
- Explicit sequential best-effort fallback batch semantics outside Android
- Explicit `platformData.rootPath` requirement for non-Android fallback `files`/`media` operations

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

See migration details in `docs/MIGRATION.md`.
Extension namespaces are documented in `docs/extensions/`.
Canonical error mapping rules are documented in `docs/ERROR_MAPPING.md`.
Cross-platform semantics are documented in `docs/API_SEMANTICS.md`.

## Development

```bash
cd packages/simple_query
flutter test
```
