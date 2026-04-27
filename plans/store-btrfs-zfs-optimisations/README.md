# Store optimisations for btrfs and ZFS

Companion to `../store-apfs-optimisations/`. Same investigative pattern,
applied to the two CoW-capable Linux filesystems most heavily used under
`/nix/store`. Several primitives — reflinks via `copy_file_range` /
`FICLONE`, `posix_fadvise` cache hints — are cross-platform with the APFS
plans, so this folder cross-references rather than duplicates.

## Existing FS-specific code

| Where | What | FS | Notes |
| --- | --- | --- | --- |
| `src/libstore/sqlite.cc:66-86` | `fdatasync` workaround on opening `db.sqlite-shm` | ZFS only | Bandaid for openzfs/zfs#14290; remove once the `zfs_putpage` upstream fix is widely deployed (~2027). |
| `tests/nixos/fsync.nix` | crash-then-mount integrity test for ext4/btrfs/xfs | btrfs+xfs+ext4 | Already exercises `fsync-store-paths`; missing ZFS. |
| `tests/functional/nars.sh:133-167` | comments around `utf8only`+`normalization` ZFS unicode collisions | ZFS | No code, just behaviour notes. |
| `src/libstore/local-store.cc:209-211` | `posix_fallocate` of `gc-reserved-space` reserve | all | Compresses to nothing on btrfs/ZFS+compression — defeats the reserve. |
| `src/libutil/file-descriptor.cc:284` | `sync_file_range(SYNC_FILE_RANGE_WRITE)` for `startFsync` | Linux generic | Already in use, btrfs/ZFS hit it. |

Nothing else. No `FICLONE`, no `chattr +C`, no btrfs subvolumes, no ZFS
dataset awareness, no cross-VFS reflink test.

## Opportunities ranked by payoff / effort

| # | Plan | Effort | Risk | Value |
|---|------|--------|------|-------|
| 01 | `chattr +C` (NoCOW) on SQLite db on btrfs | XS | Low | High — fixes long-standing fragmentation pathology |
| 02 | Make `gc-reserved-space` reserve incompressible | XS | Low | Medium — closes #3808 |
| 03 | NixOS VM test for cross-VFS reflink (btrfs) | XS | None | Low-Medium — closes #5513 (fixed but untested) |
| 04 | `FICLONE` `cloneFile` helper (Linux side of APFS plan 06) | S | Low | High — composable building block |
| 05 | Opt-in btrfs subvolume per `build-dir` | M | Med | Medium — fast cleanup for many-small-files builds |
| 06 | `posix_fadvise` cache hints (Linux side of APFS plan 04) | S | Low | Medium — cross-references `../store-apfs-optimisations/04-cache-hints.md` |
| 07 | EIO-aware `nix-store --repair-path` for ZFS scrub-detected corruption | XS | Low | Medium — closes #8121 |
| 08 | Tracking: remove ZFS `zfs_putpage` workaround once upstream fix lands | XS | None | Housekeeping |

## Dependency graph

```
01 (sqlite NOCOW)        ── independent
02 (GC reserve)          ── independent
03 (reflink VM test)     ── independent
04 (FICLONE helper)      ── shares cloneFile() with APFS plan 06; rebase whichever lands second
05 (btrfs subvol build)  ── soft-depends on 04 if we want to also reflink output → store
06 (cache hints)         ── overlaps APFS plan 04; collapse into one cross-platform PR
07 (ZFS EIO repair)      ── independent
08 (workaround removal)  ── depends on upstream openzfs/zfs fix being widely deployed
```

## Existing related upstream work

- **[PR #15410](https://github.com/NixOS/nix/pull/15410)** — `libutil/serialise: Use zero-copy copy_file_range for copying FdSource → FdSink` (Sergei Zimmerman, open). Adds `copy_file_range` in `FdSource::drainInto` with cached-`ENOSYS` fallback. Touches `src/libutil/serialise.{cc,hh}`, `src/libutil/posix-source-accessor.cc`, `src/libutil/fs-sink.cc`, `src/libutil/meson.build`. **Plan 04 here builds on top** — `copy_file_range` is the implicit-CoW fast path; `FICLONE` is the explicit-reflink primitive that returns control over the decision (useful for `du` accounting and GC contracts).
- **[PR #7126](https://github.com/NixOS/nix/pull/7126)** — `Add fsync-store-paths option` (squalus, merged 2024-08). Origin of the `sync_file_range` Linux path. Cite for context in plans 01 and 02.
- **[Issue #5513](https://github.com/NixOS/nix/issues/5513)** — "Test: Readonly nix store mount still allows for reflinks" (open). Eelco confirms in-tree fix landed in Linux 5.18; Ericson reopened explicitly to ask for a NixOS VM test. Plan 03 closes this.
- **[Issue #3808](https://github.com/NixOS/nix/issues/3808)** — "Nix daemon with /nix on full btrfs partition with compression enabled core dumps somewhere in sqlite" (open since 2019). Reporter's hypothesis is correct: zero-filled `gc-reserved-space` compresses to ~nothing. Plan 02 closes this.
- **[Issue #8121](https://github.com/NixOS/nix/issues/8121)** — "nix-store --repair-path fails on ZFS on EIO" (open). Plan 07 closes this.
- **[Issue #1272](https://github.com/NixOS/nix/issues/1272)** — "Allow some derivations to hardlink to other files in the store" (open since 2017). Original use case is now better served by reflinks (no `EXDEV` issue); plan 04 enables that path.
- **[Issue #9450](https://github.com/NixOS/nix/issues/9450)** — "Incremental store optimisation" (orthogonal; tracked in APFS plan 06).
- **openzfs/zfs `zfs_putpage` work** — referenced from `sqlite.cc:70`. Plan 08 is the tracking placeholder.

No PRs touch `chattr +C`, btrfs subvolumes, `FICLONE`, EIO handling on read, or compression-aware GC reserve.

## Coordination plan

1. PR #15410 should land first (it's small, mostly approved, and unlocks plan 04 trivially).
2. Plans 01, 02, 03, 07, 08 are independently shippable in parallel — no shared files between them.
3. Plan 04 rebases on PR #15410 once merged; reuses APFS plan 06's `cloneFile` helper if that lands first, otherwise introduces it.
4. Plan 05 should wait on PR #15410 + plan 04 so that "rename build output → store" can use a reflink instead of a rename across subvolume boundaries.
5. Plan 06 should be merged with APFS plan 04 into a single "cross-platform cache hints" PR.

## Reusable filesystem-investigation pattern

Documented for future passes (XFS, ext4, ReFS, FAT/exFAT, Lustre, …).

1. **Inventory FS-specific code** with `rg -n -i 'FSNAME|FS_SPECIFIC_SYSCALL|FS_IOCTL'` across `src/`, `tests/`, `scripts/`, `doc/`. Also grep for likely workaround comments (`workaround`, `bug`, `issue`).
2. **Audit the canonical I/O layers** in this order:
   - `src/libutil/unix/file-descriptor.cc` — sync primitives (`fsync`, `sync_file_range`, `F_FULLFSYNC`, `F_BARRIERFSYNC`).
   - `src/libutil/file-system.cc` — `copyFile`, `recursiveSync`, `copyRecursive`.
   - `src/libstore/optimise-store.cc` — dedup pass (hardlinks today; reflinks tomorrow).
   - `src/libstore/posix-fs-canonicalise.cc` — metadata stripping and immutable bits.
   - `src/libstore/unix/build/{linux,darwin,freebsd}-derivation-builder.cc` — chroot/jail setup, FOD output handling.
   - `src/libstore/local-store.cc` — startup, GC reserve, db open.
   - `src/libstore/sqlite.cc` — db file handling, FS-specific bandaids.
   - `src/libutil/archive.cc` — NAR + case-hack defaults.
   - `src/libutil/fs-sink.cc` — `RestoreRegularFile` (NAR extraction).
3. **For each FS, check the five axes:**
   1. **Reflink / CoW copy** — `FICLONE` / `copy_file_range` / btrfs subvolume / ZFS clone / APFS `clonefile`.
   2. **Cache hints** — `posix_fadvise` (Linux/FreeBSD) / `F_RDAHEAD`+`F_NOCACHE` (Darwin).
   3. **Async writeback / barrier-only fsync** — `sync_file_range` / `aio_fsync` / `F_BARRIERFSYNC` / `O_DSYNC`.
   4. **Volume capability probes** — `pathconf` / `getattrlist` / `statfs.f_type` / `statvfs`.
   5. **Hardlink limits / quirks** — `LINK_MAX`, `.app/Contents/`, HFS+ apple-double, btrfs xattr limit.
4. **Check upstream coordination** via `gh pr list --search`, `gh issue list --search`, `gh search issues`, `git log --grep`. Always cross-check the Lix fork.
5. **Plan layout** — `README.md` with the table above + dependency graph; one file per task with: Goal, Why, Existing related upstream work, Files touched, Setting design (if any), Commits in order, Availability/SDK gating, Tests, Risk and rollback, Dependencies, Definition of done.
