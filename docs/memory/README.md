# Memory — durable repo context

Repo-level memory that auto-loads into Claude Code sessions. The **repo is
the source of truth**; any mirror under
`~/.claude/projects/<repo>/memory/` is generated from this folder. If the
two disagree, this folder wins.

[`MEMORY.md`](MEMORY.md) is the one-line index. Each entry is a single file
with YAML frontmatter (`name`, `description`, `type` ∈
`user | project | feedback | reference`); for `feedback`/`project`, the
body explains the rule and how to apply it.
