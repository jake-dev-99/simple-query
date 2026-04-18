# Release flow

Three-branch promotion pipeline with pub.dev publishing gated on
`main`:

```
feature branch
     │  PR  ▼  (CI runs)
  develop
     │  PR  ▼  (CI runs)
  staging
     │  PR  ▼  (CI runs)
    main
     │  push to main ▼  (auto-tag + CD publishes)
    pub.dev
```

## Branch intent

| Branch | Role |
|---|---|
| `develop` | Default working branch. Feature branches PR here. |
| `staging` | Pre-release gate. |
| `main` | Production. Pushes trigger auto-versioning + pub.dev publishing. |

## Cutting a release

1. Land your work on `develop` via PRs.
2. `develop` -> `staging` PR. CI runs. Merge.
3. `staging` -> `main` PR. CI runs. Merge.
4. The push to `main` kicks off the auto-tag workflow, which
   computes the next version per changed package, updates
   `pubspec.yaml`, pushes a tag, and the tag push fires the
   publish workflow.

Most releases are patch bumps (auto-increment from the last
tag). To ship a minor or major release, **bump the pubspec
version explicitly** on the PR that lands the change — the
auto-tagger respects an explicit bump and publishes at that
version rather than patch-incrementing past it.

## Tag patterns (one per federated package)

| Package | Tag prefix | Working dir |
|---|---|---|
| `simple_query` | `simple_query-v` | `packages/simple_query` |
| `simple_query_platform_interface` | `simple_query_platform_interface-v` | `packages/simple_query_platform_interface` |
| `simple_query_android` | `simple_query_android-v` | `packages/simple_query_android` |
| `simple_query_ios` | `simple_query_ios-v` | `packages/simple_query_ios` |
| `simple_query_macos` | `simple_query_macos-v` | `packages/simple_query_macos` |
| `simple_query_linux` | `simple_query_linux-v` | `packages/simple_query_linux` |
| `simple_query_windows` | `simple_query_windows-v` | `packages/simple_query_windows` |
| `simple_query_shared` | `simple_query_shared-v` | `packages/simple_query_shared` |

Each package's version advances independently.

## One-time pub.dev setup (per package)

Before the first tag-triggered release, each federated package
must be configured:

1. Visit `https://pub.dev/packages/<package>/admin`.
2. Enable **Automated publishing** -> *Publishing from GitHub Actions*.
3. Fill in:
   - **Repository**: `<owner>/simple-query`
   - **Tag pattern**: `<package>-v{{version}}`
4. Save.

Without this, `dart pub publish` from the workflow errors with
`missing OIDC authorization` and the release fails cleanly.

## Why this shape

- **CI on PR opened.** Catches breakage before merge.
- **Auto-tag on push to main.** Every landed change ships as
  at least a patch bump; developers escape the auto-patch
  cadence by bumping pubspec explicitly on the PR that needs
  a minor/major release.
- **OIDC (no stored tokens).** Current pub.dev recommendation.
