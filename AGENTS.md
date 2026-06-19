# AGENTS.md — working in `simple_query`

Canonical guide for any agent or contributor in this repo (Claude Code
reads it via [`CLAUDE.md`](CLAUDE.md), which `@`-imports this file).
Keep it short and true.

## What this is

**`simple_query`** — a **federated Flutter plugin**, organized as a **melos
workspace** (`simple_query_workspace`), providing a cross-platform query
layer. It is a **Simple Zen Plugin** (a library, not a consumer app),
consumed by **Unify Messages+** as a path/pub dependency. Governance: the
Simple Zen SOP family in Notion (Documentation Standard, Code Quality
Standards, Toolchain Architecture). As `Type = Plugin`, the App-only gates
(Linear project, Figma, consumer Category, GTM/brand) do **not** apply;
code-quality, semver/API-stability, tests, and docs do.

## Layout (melos workspace)

```text
packages/
  simple_query                 # the public API package
  simple_query_platform_interface   # the contract — the load-bearing API
  simple_query_shared          # shared types
  simple_query_android | _ios | _linux | _macos | _windows   # platform impls
extensions/                    # optional query extensions
```

The **platform interface is the contract**; each platform package
implements it, and `simple_query` is the public surface.

## Build · test · verify

Dart `^3.6.0`; **melos** drives the workspace. Mirror CI
(`.github/workflows/verify.yml`), a per-package analyze + test matrix.
Before any push:

```sh
dart pub global activate melos   # if not installed
melos bootstrap                  # link the workspace packages
# then, in each touched package (as CI does):
flutter pub get
flutter analyze --no-fatal-warnings
flutter test
```

See `melos.yaml` for the workspace scripts.

## Conventions that have teeth

- **The platform-interface contract is versioned.** A breaking change to
  `simple_query_platform_interface` is a **major** bump, and every platform
  implementation moves with it in the same change — no half-migrated
  federation.
- **Bootstrap with melos** before building; cross-package path links won't
  resolve otherwise.
- `analysis_options.yaml` is the lint baseline; analyze must be clean.
- The deeper design contracts live in `docs/` — `API_SEMANTICS.md`,
  `DESIGN.md`, `ERROR_MAPPING.md`, `PLATFORM_SUPPORT_MATRIX.md`,
  `MIGRATION.md`, `EXTENSIONS.md`. Treat `API_SEMANTICS.md` and
  `ERROR_MAPPING.md` as binding contracts, not suggestions.

## Git workflow

`main`-only with git tags for releases (no `develop`/`staging`). One
short-lived branch per work item; PRs target `main`. Releases are cut via
`.github/workflows/release.yml`; see [`docs/RELEASE.md`](docs/RELEASE.md).

## What NOT to do (binding rulings)

- **Don't make `simple_query` depend on `simple_permissions`.** `simple_query`
  is permission-*aware* — it checks state and **fails fast** with
  `SimpleQueryError(code: permissionDenied, …)` carrying the OS permission
  id — but it never *requests* permissions, never depends on
  `simple_permissions`/`simple_permissions_native`, and never surfaces their
  types in its public API. (Past guidance was the opposite; this is
  inverted intentionally.) Flag any `simple_permissions*` import in
  `packages/simple_query*/` or any direct `requestPermissions` /
  `*requestAccess` / `*requestAuthorization` call. See
  [`docs/memory/feedback_permissions.md`](docs/memory/feedback_permissions.md).
- **Don't break the platform-interface API without a major version bump** —
  downstream (Unify Messages+) and pub.dev consumers depend on it.
- **Don't change documented error mappings or query semantics silently** —
  `ERROR_MAPPING.md` / `API_SEMANTICS.md` are the contract; update them in
  the same change and bump accordingly.
- **Don't build without `melos bootstrap`** — you'll chase phantom
  unresolved-path errors.
- **Don't push without the verify gate green** (`analyze --no-fatal-warnings`
  + `test` across touched packages). CI is a backstop, not discovery.
- **Don't commit secrets.**
- **Don't add app-level concerns** here (GTM, brand, product roadmap) —
  this is a library.
