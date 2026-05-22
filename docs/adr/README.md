# Architecture Decision Records

Short records of the load-bearing decisions behind `simple_query`. Each ADR
captures **context · decision · alternatives considered · consequences ·
resolution trigger** (the last only when the decision is bounded). New
decisions land here as numbered files (`NNNN-kebab-title.md`).

| # | Decision | Status |
| --- | --- | --- |
| _none yet_ | — | — |

## Prior design notes to formalize

These existing docs capture decisions that predate this directory and
should be folded into numbered ADRs as they're revisited:

- [`../DESIGN.md`](../DESIGN.md) — overall design rationale.
- [`../API_SEMANTICS.md`](../API_SEMANTICS.md) — query API semantics (a binding contract).
- [`../ERROR_MAPPING.md`](../ERROR_MAPPING.md) — error-mapping contract.
- [`../PLATFORM_SUPPORT_MATRIX.md`](../PLATFORM_SUPPORT_MATRIX.md) — per-platform support decisions.
- [`../MIGRATION.md`](../MIGRATION.md) — migration guidance.

> As a Plugin (not an App), `simple-query` has no per-concern Notion docs —
> these records are self-contained here.
