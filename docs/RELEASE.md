# Release tooling

Release execution, dependency rollout, bump choices, and recovery procedure:
[UNFY-345 in Linear](https://linear.app/simple-zen/issue/UNFY-345/repair-plugin-dependency-floors-and-telephony-release-example).

`tool/release-packages.json` defines package ownership. `release_plan.py` selects
owned changes and affected dependents and freezes manifests/changelogs.
`release_coordinator.py` records authorized batches in draft GitHub Releases,
validates source/hosted graphs, and verifies uploaded SDK archives in dependency order.
`release_services.py` honors metadata cache lifetimes and quota deadlines;
`release_github.py` observes and recovers original tag-push Deploy runs.
`release_repository.py` rejects authored dependency overrides before validation.

`.github/workflows/release.yml` connects manual cuts and scheduled recovery to the
coordinator. `.github/workflows/deploy.yml` retains package tag-push OIDC publication.
`PUB_MAX_HTTP_RETRIES=1` disables SDK HTTP retries so quota recovery belongs to the
coordinator. The app token triggers tag workflows; `GITHUB_TOKEN` handles journals
and Actions recovery. Configuration keys are `RELEASE_APP_ID` and
`RELEASE_APP_PRIVATE_KEY`. Package tags follow `<package>-v<version>`.

| Package | Directory |
| --- | --- |
| `simple_query` | `packages/simple_query` |
| `simple_query_platform_interface` | `packages/simple_query_platform_interface` |
| `simple_query_android` | `packages/simple_query_android` |
| `simple_query_ios` | `packages/simple_query_ios` |
| `simple_query_macos` | `packages/simple_query_macos` |
| `simple_query_linux` | `packages/simple_query_linux` |
| `simple_query_windows` | `packages/simple_query_windows` |
| `simple_query_shared` | `packages/simple_query_shared` |
