# Release flow

A merge to `main` does not publish. A manual **Cut Release** dispatch authorizes
one frozen batch; package tag pushes run **Deploy**, which publishes with OIDC.

## Cutting a batch (UNFY-345)

1. Merge the reviewed PRs into `main` and complete any cross-repository prerequisites.
2. Dispatch [Cut Release](../.github/workflows/release.yml) on `main` with
   `operation=cut`, `package=all`, and the appropriate `patch`, `minor`, or `major` bump.
   Use `dry_run=true` first to inspect selection reasons, versions, floors, notes,
   and dependency order, and to validate the candidate source graph.
3. `all` selects packages with owned release changes since their last tag whose
   version is also present on pub.dev, plus dependents needing raised floors.
   Unchanged unrelated packages are skipped. Root facade ownership excludes nested
   platforms and release tooling. Individual package cuts remain available.
4. The chosen increment applies to every selected package's current source version;
   versions already edited in a PR also receive that increment. The planner raises
   internal dependency floors, preserves authored `Unreleased` notes, and refuses
   constraint downgrades or cyclic dependencies. Review breaking APIs and select
   `major` when the platform-interface contract requires it.
5. Source preflight runs in a disposable snapshot with only intentional batch links;
   external dependencies still resolve from pub.dev. The workflow freezes the source
   SHA, files, versions, tags, and prepared commit in an internal draft GitHub Release.
   It pushes that commit once, then processes packages in dependency order.
6. Before each package tag, clean hosted resolution (including its bundled example),
   analysis, tests when present, and `flutter pub publish --dry-run` must pass.
   Only then is that tag pushed. The coordinator observes its original tag-push
   Deploy run and verifies the exact hosted version, archive checksum, and frozen
   source contents before advancing to a dependent package. Deploy remains the
   uploader; the coordinator never uploads directly.

A dependency within the same batch may start unpublished: its package is published
and confirmed first. A repository's `all` cannot release another repository. Missing
external versions stop validation and require completing that prerequisite first.

## Pauses and automatic recovery

HTTP requests honor server cache lifetimes and `Retry-After` deadlines. Known
publication timestamps apply conservative rolling quotas: per package 3 in two
minutes, 6 in an hour, and 12 in a day; known prior batch uploads also account for
200 per publishing agent in each window. These are upstream production configuration
values, not a promise about remaining deployed quota; other repositories and manual
uploads are not visible locally. Server rate-limit responses remain authoritative.

An upload quota saves `status=deferred`, the retry deadline, exact original Deploy
run, and progress. When Flutter exposes only the quota message, the coordinator waits
the full reported two-minute, one-hour, or one-day window. No retry occurs before that
deadline. The hourly scheduled workflow automatically resumes the authorized batch
on its first eligible run, rerunning failed jobs of the original tag-push Deploy run.
Long cooldowns use the durable journal instead of keeping a runner alive for a day.
Completed packages are never uploaded again. Short transient upload failures use
bounded backoff and jitter; authentication/validation failures require attention.
An unrecognized quota window pauses for attention without speculative uploads.

The Actions summary and internal draft release show progress and the reason for a
pause. The immutable `sz-release-batch-<run-id>` tag records manual intent and survives
an interrupted journal creation; it matches no package Deploy trigger. Scheduled
runs select no new changes. Completed, cancelled, and blocked batches do no uploads.

To continue a blocked batch after fixing its external prerequisite, dispatch
`operation=resume` on `main`. It keeps the frozen versions and source; it does not
adopt new source fixes. A new cut is rejected while another batch is incomplete.
`operation=cancel` stops remaining work; it cannot stop an already running Deploy
or undo a published package. A cancelled batch requires inspecting any in-flight
Deploy before authorizing another cut. Dry runs never change the journal.

Never delete, move, or recreate package tags to recover. Prefer batch resume or
GitHub's **Re-run failed jobs** on the original tag-push Deploy run. A new manual
Deploy dispatch does not preserve pub.dev's required `push` OIDC event claim.

## Publication setup

`RELEASE_APP_ID` / `RELEASE_APP_PRIVATE_KEY` identify the app with contents write
and the existing `main` ruleset bypass. Its pushes trigger downstream Deploy;
`GITHUB_TOKEN` supplies contents write for the draft journal and actions write for
original-run retries. All Cut Release and scheduled resume runs share one repository
concurrency group. Initial release dispatches and merges remain human operations.

Each package must already exist on pub.dev and have automated publishing configured
for this repository with tag pattern `<package>-v{{version}}`. Pub.dev cannot create
a new package through automated publishing; an authorized uploader must publish its
first version interactively from the reviewed source without development overrides.

See [Dart automated publishing](https://dart.dev/tools/pub/automated-publishing),
[pub.dev hosted API](https://pub.dev/help/api), and the verified upstream
[quota configuration](https://github.com/dart-lang/pub-dev/blob/95314cb7355cc4f57a6ab55b1929a40693ee7716/app/config/dartlang-pub.yaml).

## Package tags

| Package | Tag prefix | Directory |
| --- | --- | --- |
| `simple_query` | `simple_query-v` | `packages/simple_query` |
| `simple_query_platform_interface` | `simple_query_platform_interface-v` | `packages/simple_query_platform_interface` |
| `simple_query_android` | `simple_query_android-v` | `packages/simple_query_android` |
| `simple_query_ios` | `simple_query_ios-v` | `packages/simple_query_ios` |
| `simple_query_macos` | `simple_query_macos-v` | `packages/simple_query_macos` |
| `simple_query_linux` | `simple_query_linux-v` | `packages/simple_query_linux` |
| `simple_query_windows` | `simple_query_windows-v` | `packages/simple_query_windows` |
| `simple_query_shared` | `simple_query_shared-v` | `packages/simple_query_shared` |

## Current dependency rollout

Use one `all` **minor** cut to publish changed Query packages and their affected
dependents. The Android and public Query sources require the `0.3.0` generation;
a patch candidate below an existing required floor is rejected. Review the dry-run
plan: additional changed interface/shared/platform packages may be included and
published first. Confirm Query `0.3.0` before Telephony or SMS releases.
