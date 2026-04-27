# 08 — Tracking: remove the ZFS `zfs_putpage` SQLite workaround

## Goal

Tracking-only plan. Remove the workaround at
`src/libstore/sqlite.cc:66-86` once the upstream `zfs_putpage` fix is
widely deployed. No code change required *yet*; this plan documents the
removal criteria and the verification recipe so a future contributor
doesn't have to re-derive them.

## Why

The block in `sqlite.cc` does an extra `fdatasync(db.sqlite-shm)` at
SQLite open time on ZFS to work around
[openzfs/zfs#14290](https://github.com/openzfs/zfs/issues/14290), which
caused SQLite's `truncate()` call on `db.sqlite-shm` to randomly take
multiple seconds. The comment at line 69 says:

> Remove this workaround when a fix is widely installed, perhaps 2027? Candidate:
> https://github.com/search?q=repo%3Aopenzfs%2Fzfs+%22Linux%3A+zfs_putpage%3A+complete+async+page+writeback+immediately%22&type=commits

The candidate fix is the
[`zfs_putpage` async page-writeback completion change](https://github.com/openzfs/zfs/commit/?q=%22Linux%3A+zfs_putpage%3A+complete+async+page+writeback+immediately%22).
Once that lands in OpenZFS releases that ship in supported NixOS
versions, the workaround is dead weight.

## When to act

Trigger conditions, all required:

1. The OpenZFS fix is merged to `master`.
2. It has shipped in a tagged OpenZFS release (e.g. ≥ 2.3.0).
3. NixOS stable has been bumped to that release for at least one cycle
   (e.g. landed in `release-24.11` and verified by Hydra).
4. No regression reports against the `db.sqlite-shm` slow-truncate
   pattern in the past two release cycles.

Use the GitHub-search query in the existing comment to track #1; for
#2-#4, check `pkgs/os-specific/linux/zfs/` in Nixpkgs.

## How to verify before removal

1. On a NixOS box with the fixed ZFS, mount a ZFS dataset, point
   `/nix` at it (or use `--store`).
2. Strip the workaround locally; re-run the daemon.
3. Run `time nix-store --gc` (or any operation that opens the DB) ten
   times in a tight loop. Confirm no occurrence > 1s on the SQLite
   open path.
4. Cross-check by enabling `NIX_DEBUG_SQLITE_TRACES=1` to make sure
   the open isn't being elided.

## Files touched (when removed)

- `src/libstore/sqlite.cc` — delete the `#ifdef __linux__` block at
  `:66-86` and its dependent includes (`<sys/statfs.h>` if not used
  elsewhere).
- `src/libstore/meson.build` — drop `fstatfs` from `check_funcs` if
  not referenced elsewhere.
- `doc/manual/rl-next/remove-zfs-sqlite-workaround.md` — release note.
- *(no test file change — the existing `tests/nixos/fsync.nix` doesn't
  exercise ZFS today; if plan 01 or this plan motivates adding ZFS to
  that matrix, do it then)*.

## Commits (in order, **future**)

1. **`libstore: remove ZFS SQLite db.sqlite-shm fdatasync workaround`**
   - Delete the block. Cite the merged OpenZFS commit hash in the
     commit body. Reference issue #14290 and this plan.

2. **`doc: release note for ZFS workaround removal`**
   - "Removed the legacy ZFS-specific `fdatasync` workaround in
     `SQLite::SQLite`. It is no longer needed on OpenZFS ≥ X.Y.Z."

## Risk and rollback

Risk when removed: low if the trigger conditions are met. The workaround
is a small extra `fdatasync`; removing it has zero behaviour change on
non-ZFS systems and only matters where the bug was previously
mitigated.

Rollback: trivial revert if a regression appears.

## Dependencies

- External: OpenZFS upstream timeline.
- None internal.
- Plan 01 (`chattr +C` on SQLite db) is harmless to coexist with this
  workaround indefinitely; do not block plan 01 on this one.

## Definition of done

This plan is "done" when the workaround block is removed. Until then,
this file's purpose is purely to capture the criteria so the decision
isn't lost.

- [ ] Trigger conditions verified (see "When to act").
- [ ] Workaround removed; release note merged.
- [ ] Manual verification completed (see "How to verify").
