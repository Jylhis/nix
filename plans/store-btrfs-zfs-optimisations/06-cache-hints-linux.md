# 06 — Cache hints for Linux (`posix_fadvise`) — collapse with APFS plan 04

## Goal

Wire `posix_fadvise(POSIX_FADV_SEQUENTIAL)` and
`posix_fadvise(POSIX_FADV_DONTNEED)` into the same call sites that APFS
plan 04 (`../store-apfs-optimisations/04-cache-hints.md`) hooks for
Darwin: `recursiveSync`, `PosixSourceAccessor::readFile`, and
`RestoreRegularFile::preallocateContents` for very large NARs.

## Why

The motivation is identical to the APFS plan: page-cache pollution
during `nix copy` and large NAR restore evicts other useful pages.
Linux's `posix_fadvise` is the closest analogue to Darwin's `F_RDAHEAD`
/ `F_NOCACHE`, and conveniently is a strict superset:

| Hint | Darwin | Linux |
|---|---|---|
| Read-ahead enable | `fcntl(F_RDAHEAD, 1)` | `posix_fadvise(POSIX_FADV_SEQUENTIAL)` |
| Drop after use | `fcntl(F_NOCACHE, 1)` (write side) / `mmap` MADV_DONTNEED (read side) | `posix_fadvise(POSIX_FADV_DONTNEED)` |
| Sequential read | `F_RDAHEAD` (with `F_RDADVISE` for ranges) | `POSIX_FADV_SEQUENTIAL` |

On btrfs, `POSIX_FADV_DONTNEED` is particularly valuable: btrfs's
delayed-allocation + compression workflow holds modified pages in cache
for longer than other FS, and the GC pressure from a `nix copy` of a
multi-GB closure (e.g. Chromium with debug symbols) can evict the
working set of unrelated processes.

ZFS uses ARC, not the kernel page cache, so `POSIX_FADV_DONTNEED` is
mostly a no-op there — but harmless. The hints are best-effort and
swallow errors universally.

## Existing related upstream work

- **APFS plan 04** — this plan should be **collapsed into a single
  cross-platform PR** with that one. Same call sites, same setting
  (`darwin-write-nocache-threshold` would be renamed to
  `cache-hint-large-write-threshold` to drop the platform prefix), same
  test matrix.
- No upstream PR.
- [PR #14991](https://github.com/NixOS/nix/pull/14991) "Process-performance
  improvements" (Connor Baker, open) touches adjacent perf knobs but
  not cache hints; coordinate to avoid textual conflicts.

## Files touched

Same files as APFS plan 04. Specifically:

- `src/libutil/include/nix/util/file-descriptor.hh` — `void hintSequential(Descriptor)`, `void hintDontNeed(Descriptor)`, `void hintNoCacheWrite(Descriptor)` (existing function trio).
- `src/libutil/file-descriptor.cc` (cross-plat shim).
- `src/libutil/unix/file-descriptor.cc` — Linux body uses `posix_fadvise`; Darwin body uses `fcntl(F_RDAHEAD)` / `fcntl(F_NOCACHE)`.
- `src/libutil/file-system.cc` — `recursiveSync` callsite.
- `src/libutil/posix-source-accessor.cc` — `readFile` callsite.
- `src/libutil/fs-sink.cc` — `preallocateContents` callsite.
- `src/libstore/include/nix/store/local-settings.hh` — `cache-hint-large-write-threshold` setting (default 64 MiB).
- `src/libutil-tests/unix/file-descriptor.cc` — unit smoke tests.
- `doc/manual/rl-next/cache-hints.md` — single release note covering both platforms.

## Commits (in order)

These are the **same commits as APFS plan 04**, with Linux bodies added
to commit 1 instead of Darwin-only no-ops. If APFS plan 04 has already
landed Darwin-only, this plan is a single commit:

1. **`libutil: add Linux posix_fadvise bodies to existing cache-hint helpers`**
   - In each of `hintSequential`, `hintDontNeed`, `hintNoCacheWrite`:
     - `__APPLE__` branch: existing `fcntl` calls (from APFS plan 04).
     - `__linux__` branch: `posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL/DONTNEED/NOREUSE)`.
     - Else: existing no-op.
   - Failure-swallowing semantics preserved.

If APFS plan 04 has *not* landed yet, the canonical PR is APFS plan 04 +
this plan **as one** — same five commits, same test plan, just Linux-side
bodies added to commit 1.

## Tests

- Unit smoke tests as in APFS plan 04, extended to assert no error on Linux.
- Functional: same 1 GB random-bytes derivation `nix copy --to ./store` test; on Linux, optional `vm_stat`-equivalent check via `/proc/meminfo` `Cached:` deltas (flaky in CI; manual).
- Benchmark: PR description should include numbers from both Darwin (APFS) and Linux (btrfs + ext4 baselines).

## Risk and rollback

Risk: low (same as APFS plan 04). `POSIX_FADV_DONTNEED` is best-effort
and on a busy system the kernel may ignore it; that's fine.

Rollback: revert commit 1; helpers stay defined as no-ops.

## Dependencies

- **Hard recommendation**: do **not** ship this plan separately. Merge
  it into APFS plan 04 and ship one cross-platform PR.
- Independent of plans 01-05, 07, 08 here.

## Definition of done

- [ ] If shipped as part of APFS plan 04 single PR: that PR's
      definition-of-done covers this plan.
- [ ] If shipped separately: same DoD as APFS plan 04, with Linux
      benchmark numbers added.
- [ ] Setting `cache-hint-large-write-threshold` documented in the
      manual under "Local store".
- [ ] Release note merged.
