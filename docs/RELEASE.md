# Release flow

`main`-only with manual cut + tag-driven pub.dev publishing, per the
Simple Zen Toolchain Architecture SOP.

```
feature branch
     │  PR  ▼  (CI runs)
    main
     │  release.yml workflow_dispatch ▼  (cuts release, bumps + tags)
     │  tag push ▼                       (deploy.yml runs OIDC pub.dev release)
    pub.dev
```

## Branch intent

| Branch | Role |
|---|---|
| `main` | Production. Feature branches PR directly here. The release workflow is dispatched manually on `main`; the resulting tag push triggers the pub.dev publish. |

`develop` / `staging` branches were retired — solo-dev ops doesn't justify
a multi-branch promotion pipeline. CI runs on every PR; CD runs on **tag
push**, not on the merge.

## Cutting a release

1. Land your work on `main` via a PR. CI runs; merge.
2. Dispatch [`release.yml`](../.github/workflows/release.yml) on `main`
   via the GitHub Actions UI (`workflow_dispatch`). It computes the
   next version for each selected package and validates published dependencies without workspace overrides.
   It then updates `pubspec.yaml` and CHANGELOG, commits, tags, and pushes.
3. Each tag push fires [`deploy.yml`](../.github/workflows/deploy.yml) for the matching package.
   It verifies ancestry and version, configures OIDC credentials, and runs `flutter pub publish --force` with normal validation.

Choose `patch`, `minor`, or `major` in the workflow input.
The workflow increments the current pubspec version by that choice, including versions already changed in a PR.
Use `dry_run=true` to inspect the candidate and validate published dependencies without pushing a commit or tag.

### MMS dependency release order (UNFY-123)

Android and the façade require `simple_query_platform_interface ^0.2.1` for `QueryFieldCatalog` and its runtime validation.
Published interface `0.2.0` lacks that API.
The façade also requires `simple_query_android ^0.2.2` for the native ContentQuery implementation consumed by simple-sms.

Release these packages separately, waiting for each version to appear on pub.dev before cutting its dependent package:

1. `simple_query_platform_interface`: patch from `0.2.1` to `0.2.2`.
2. `simple_query_android`: patch from `0.2.2` to `0.2.3`.
3. `simple_query`: patch from `0.2.2` to `0.2.3`.

Do not select `all` while selected packages require unpublished dependencies.
The cut stops before tagging when dependency resolution or analysis fails.
Development overrides prove local compatibility; they do not prove the published dependency graph.

## CHANGELOG discipline

Every package's `CHANGELOG.md` starts with a `## Unreleased` section.
Every PR that changes behaviour in that package appends bullets under
it.

The release workflow prepends a version heading and commit summaries scoped to the selected package.
Review those summaries through the dry run before cutting.
`tool/publish.sh` is a separate legacy workspace publisher; use the tag-driven workflow described here for this dependency release.

After a release lands on `main`, the next PR re-seeds `## Unreleased`
at the top of each published CHANGELOG.

This keeps the release notes truthful (every shipped change is named)
without forcing every PR to pick a version number.

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

Before the first tag-triggered release, each federated package must be
configured:

1. Visit `https://pub.dev/packages/<package>/admin`.
2. Enable **Automated publishing** → *Publishing from GitHub Actions*.
3. Fill in:
   - **Repository**: `<owner>/simple-query`
   - **Tag pattern**: `<package>-v{{version}}`
4. Save.

Without this configuration, publishing cannot authenticate with the package's GitHub binding.

To retry an existing tag, dispatch Deploy on that tag ref, with the same tag input.
For example: `gh workflow run deploy.yml --ref simple_query_android-v0.2.3 -f tag=simple_query_android-v0.2.3`.
Keep the existing tag and version when retrying a failed publication.

## Why this shape

- **CI on PR opened.** Catches breakage before merge.
- **Manual release dispatch.** Cuts are explicit — a merge to `main`
  is not automatically a release. Solo-dev workflow doesn't need the
  cadence forcing function that an auto-tag-on-merge model provides.
- **CD on tag.** The tag is the intent-to-release signal; the deploy
  workflow is the only path to pub.dev.
- **OIDC (no stored tokens).** Current pub.dev recommendation.
