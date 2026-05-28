# Runbooks

Operational procedures for `simple_query`.

| Runbook | For |
| --- | --- |
| [../RELEASE.md](../RELEASE.md) | Release/publish procedure (kept at `docs/` root for now). |

Release + deploy run from `main` only. `.github/workflows/release.yml`
cuts the release (manual `workflow_dispatch`), and
`.github/workflows/deploy.yml` triggers on the resulting tag push to run
the OIDC pub.dev publish. Full detail in [`../RELEASE.md`](../RELEASE.md).
Add rollback / incident procedures here as they're needed. For day-to-day
build/test/verify commands (incl. `melos bootstrap`), see
[`AGENTS.md`](../../AGENTS.md).
