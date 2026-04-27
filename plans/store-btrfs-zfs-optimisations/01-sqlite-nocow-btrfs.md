# 01 — `chattr +C` (NoCOW) on SQLite database files on btrfs

## Goal

When `LocalStore` opens its SQLite database on a btrfs volume, set the
`FS_NOCOW_FL` inode flag on `db.sqlite`, `db.sqlite-wal`, and
`db.sqlite-shm` if the files were just created (i.e. the daemon is
initialising a fresh store). Existing files are not touched — the flag
only takes effect on inodes with no extents, so retro-fitting requires a
copy-then-rename which we explicitly choose not to do.

## Why

SQLite + btrfs without NoCOW is a well-known fragmentation pathology.
Random in-place writes to a CoW database generate one extent per page
write; after a few weeks of normal operation `db.sqlite` ends up with
tens of thousands of fragments. `nix-store --gc`, `nix-build`, and any
operation that walks the database becomes I/O-bound on extent metadata.

The fix recommended by both
[btrfs upstream](https://btrfs.readthedocs.io/en/latest/Administration.html#mount-options)
and the [SQLite-on-btrfs FAQ](https://sqlite.org/forum/forumpost/c1bc81b2010ec)
is `chattr +C` on the database file *before any data is written*. Then
btrfs treats the file like ext4: in-place overwrites, no extent
proliferation, no fragmentation. The trade-off is no checksumming on
those specific files — acceptable for a SQLite WAL that has its own
integrity machinery.

The same flag is harmless on every other Linux filesystem (the ioctl
returns `EOPNOTSUPP` and we ignore it), and acts as a no-op on ZFS
(which has its own CoW model and isn't affected by this pathology
because ZIL absorbs the random writes).

## Existing related upstream work

- [Issue #3808](https://github.com/NixOS/nix/issues/3808) "Nix daemon
  with /nix on full btrfs partition with compression enabled core dumps
  somewhere in sqlite" — distinct symptom (compression, not CoW), but
  same family. Cite as related; plan 02 closes that one.
- [PR #7126](https://github.com/NixOS/nix/pull/7126) "Add
  fsync-store-paths option" — touches the same SQLite-on-Linux story;
  established the pattern of FS-specific bandaids in `sqlite.cc`.
- No existing PR or issue tracks NoCOW for the database.

## Files touched

- `src/libstore/sqlite.cc` — call site at construction; existing ZFS
  workaround at `:66-86` is the natural neighbour.
- `src/libutil/include/nix/util/file-system.hh` +
  `src/libutil/unix/file-system.cc` — `void disableCoW(Descriptor)`
  helper. Returns void; swallows `EOPNOTSUPP` and `ENOTTY`. Cross-platform
  signature; no-op outside Linux.
- `src/libutil-tests/file-system.cc` — unit smoke test on a temp file
  (verifies the call doesn't error on tmpfs).
- `tests/nixos/fsync.nix` — extend to additionally assert that on btrfs
  the new `db.sqlite` has the NoCOW flag set (`lsattr | grep C`).
- `doc/manual/rl-next/sqlite-nocow-btrfs.md` — release note.

## Commits (in order)

1. **`libutil: add disableCoW helper`**
   - Linux: `ioctl(fd, FS_IOC_GETFLAGS)`, set `FS_NOCOW_FL`, `ioctl(fd, FS_IOC_SETFLAGS)`. Swallow `EOPNOTSUPP`, `ENOTTY`, `EPERM`.
   - Other Unix / Windows: empty inline.
   - Unit smoke test that calls the helper on a temp file in `/tmp` (will succeed on btrfs runners, no-op elsewhere — assertion is "doesn't throw").

2. **`libstore: apply disableCoW to fresh SQLite database files on btrfs`**
   - In `SQLite::SQLite(...)`, after the existing ZFS workaround block but before `sqlite3_open_v2`, check `fstatfs(fd, &fs)`; if `fs.f_type == BTRFS_SUPER_MAGIC` (`0x9123683E`) **and** the database file did not exist before (the `LocalStore` ctor at `local-store.cc:516` knows whether it's creating from scratch), call `disableCoW` on `db.sqlite`, `db.sqlite-wal`, and `db.sqlite-shm` after they're created by SQLite's first transaction.
   - Wire the "fresh-create" signal through `SQLite::Settings` rather than re-statting.
   - Log at `lvlTalkative`: `"applied NoCOW (chattr +C) to %s on btrfs"`.

3. **`tests: extend fsync NixOS test to verify NoCOW on btrfs`**
   - Add a step in `tests/nixos/fsync.nix`: after the first `nix copy` on the btrfs scratch volume, `assert "C" in machine.succeed("lsattr /mnt/var/nix/db/db.sqlite")`.

4. **`doc: release note for SQLite-on-btrfs NoCOW`**
   - Brief note explaining the change, why it matters, and how to manually retro-fit existing stores: stop the daemon, `cp --reflink=never db.sqlite db.sqlite.tmp`, `chattr +C db.sqlite.tmp`, `mv db.sqlite.tmp db.sqlite`, `mv db.sqlite-wal /tmp/`, restart.

## Tests

- Unit smoke test (commit 1).
- NixOS VM extension (commit 3).
- Manual: on a real btrfs `/nix`, `lsattr /nix/var/nix/db/db.sqlite` after first daemon startup; `filefrag` before and after a week of normal use to confirm fragmentation stays bounded.

## Risk and rollback

Risk: very low.

- Failure modes: `ioctl` failure → silently no-op. The SQLite database remains correct either way; only fragmentation behaviour differs.
- The flag does not affect existing data — only future writes to the inode. Setting it on an empty inode is the safe path; setting it on a non-empty inode silently does nothing in btrfs and is therefore not useful retro-fitting.
- We deliberately do **not** auto-rewrite existing databases; that would require coordinating with a running daemon and is high-risk for low gain. Document the manual procedure instead.

Rollback: revert commit 2; commits 1 and 3 are independently useful and harmless.

## Dependencies

- None on the APFS plans.
- Independent of plans 02-08 here.

## Definition of done

- [ ] All commits land in order.
- [ ] Unit smoke test green on Linux + macOS CI.
- [ ] NixOS test green on btrfs.
- [ ] Release note merged with manual retro-fit recipe.
- [ ] Verified by hand on a btrfs `/nix` that `lsattr db.sqlite` shows `C` after a fresh daemon init.
