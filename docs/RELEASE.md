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
   next version per changed package, updates `pubspec.yaml`, commits,
   tags, and pushes.
3. Each tag push fires [`deploy.yml`](../.github/workflows/deploy.yml)
   for the matching package, which verifies the tag version matches
   `pubspec.yaml` and runs `dart pub publish --force` via OIDC.

Most releases are patch bumps (auto-increment from the last tag). To
ship a minor or major release, **bump the pubspec version explicitly**
on the PR that lands the change — the release workflow respects an
explicit bump and publishes at that version rather than
patch-incrementing past it.

## CHANGELOG discipline

Every package's `CHANGELOG.md` starts with a `## Unreleased` section.
Every PR that changes behaviour in that package appends bullets under
it.

On release (step 2 above), the cut-a-release flow replaces
`## Unreleased` with `## <version>` for each package being published.
`tool/publish.sh` validates that the target version has a matching
`## <version>` heading before talking to pub.dev.

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

Without this, `dart pub publish` from the workflow errors with
`missing OIDC authorization` and the release fails cleanly.

## Why this shape

- **CI on PR opened.** Catches breakage before merge.
- **Manual release dispatch.** Cuts are explicit — a merge to `main`
  is not automatically a release. Solo-dev workflow doesn't need the
  cadence forcing function that an auto-tag-on-merge model provides.
- **CD on tag.** The tag is the intent-to-release signal; the deploy
  workflow is the only path to pub.dev.
- **OIDC (no stored tokens).** Current pub.dev recommendation.
