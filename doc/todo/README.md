# Issue / TODO tracker

This folder is a lightweight file-based issue tracker for the project.
Each issue is one Markdown file.

## File naming

`NNN-short-kebab-case-title.md` — e.g. `001-bms-request-full-charge.md`.
`NNN` is a zero-padded sequence number; take the next free one. The number
never changes, even if the title is later reworded.

**A closed issue is renamed with a leading underscore**: `_NNN-…md`, e.g.
`_004-move-can-rx-off-gpio0.md`. `_` sorts after the digits, so every listing of
this folder puts the open items first and the closed ones below them, without
anything having to read the frontmatter. Rename with `git mv` when setting
`status: done` or `wontfix`, and fix the links that point at the file — other
docs link to these by filename.

## File structure

Each issue file starts with a YAML frontmatter block:

```yaml
---
id: 001
title: One-line summary of the issue
type: bug | enhancement | task
status: open | in-progress | done | wontfix
priority: high | medium | low
component: boiler | inverter | battery-can | battery-console | display | web | infra | ...
created: YYYY-MM-DD
resolved: YYYY-MM-DD   # only when status is done/wontfix
---
```

followed by these sections (omit those that do not apply):

- **Summary** — what is wrong / what is wanted, a few sentences.
- **Observed behavior** — what actually happened, with log excerpts,
  timestamps, affected files (for bugs).
- **Expected behavior** — what should have happened.
- **Root cause** — analysis result, with code references (`relay.cpp:250`).
- **Why not now** — for items parked on purpose: the conditions that make it
  low priority, and what would change that. Keeping this explicit is the point
  of parking an item rather than dropping it.
- **Proposed fix** — suggested approach, alternatives, open questions.
- **Resolution** — filled in when closing: what was done, commit reference.

## Rules

- English only (same as the rest of the repo).
- Never paste anything from `include/credentials.h` — no tokens, keys or
  passwords. WireGuard addresses used elsewhere in the repo docs are fine.
- Closed issues stay in this folder with `status: done` and the `_` prefix; do
  not delete them.
