# 04 — Cache hints for bulk read/sync passes

## Goal

Stop blowing out the page cache during the two predictably-streaming passes Nix performs: `recursiveSync()` (visits every file in a tree only to fsync it) and NAR serialisation (`SourceAccessor::dumpPath` reads each file once, sequentially).

## Why

`recursiveSync` (`src/libutil/file-system.cc:335-377`) opens every regular file in a path tree, calls `fsync`, and moves on. On a large derivation output (Chromium, Linux kernel, …) this faults in every page just to flush metadata, evicting genuinely useful pages from the buffer cache. Same story for `dumpPath` during `nix copy` and `restorePath` while writing very large files.

Both kernels offer hints for exactly this:

| Linux | Darwin |
|---|---|
| `posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL)` before sequential read | `fcntl(fd, F_RDAHEAD, 1)` before sequential read |
| `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` after we're done with a file | `fcntl(fd, F_NOCACHE, 1)` before bulk write or before `fsync`-and-discard |

These are no-ops on filesystems that don't support them, so the change is purely additive.

## Existing related upstream work

- None on the cache-hint side.
- Related infrastructure: `RestoreSink` already has `preallocateContents` which is gated by `restoreSinkSettings.preallocateContents` — cache-hint gating can follow the same pattern.

## Files touched

- `src/libutil/include/nix/util/file-descriptor.hh` — add `void hintSequential(Descriptor)`, `void hintDontNeed(Descriptor)`, `void hintNoCacheWrite(Descriptor)`.
- `src/libutil/file-descriptor.cc` (cross-plat shims) and `src/libutil/unix/file-descriptor.cc` (Unix impl).
- `src/libutil/file-system.cc` — call `hintDontNeed` in `recursiveSync`'s per-file fsync loop.
- `src/libutil/posix-source-accessor.cc` — call `hintSequential` when opening files for `dumpPath`-style sequential reads.
- `src/libutil-tests/file-descriptor.cc` — unit smoke tests.
- `doc/manual/rl-next/cache-hints.md` — release note.

## Commits (in order)

1. **`libutil: add fd cache-hint helpers`**
   - Three small functions; on Linux they wrap `posix_fadvise`, on Darwin they wrap `fcntl(F_RDAHEAD)` / `fcntl(F_NOCACHE)`. On other Unixes they are no-ops.
   - All three swallow errors (best-effort).
   - Unit smoke test verifying they don't error on a temp file.

2. **`libutil: hint DONT_NEED in recursiveSync`**
   - In the per-file fsync loop, after `fd.fsync()` call `hintDontNeed(fd)`.
   - Behaviour-preserving in correctness; observable as lower buffer-cache pressure.

3. **`libutil: hint SEQUENTIAL during dumpPath read`**
   - In `PosixSourceAccessor::readFile` (or the callsite that reads the entire file linearly), set the SEQUENTIAL hint on the FD right after `open`.
   - Touch the FD-opening helper so any future caller benefits.

4. **`libutil: opt-in F_NOCACHE for very large NAR writes`**
   - In `RestoreRegularFile::preallocateContents`, when `len` exceeds a threshold (default 64 MiB; setting `darwin-write-nocache-threshold`), call `hintNoCacheWrite(fd)` before writes start.
   - Coordinate with plan 02 — same threshold idea, different direction (write vs sync). If plan 02 lands first, reuse its setting.

5. **`doc: release note for cache hints`**
   - Brief note: lower cache pressure during `nix copy` and large path installs; tunable via the new threshold setting; opt-out is "set threshold to a very large number".

## Tests

- Unit: smoke tests as in commit 1.
- Functional: write a 1 GB random-bytes derivation, run `nix copy --to ./store`, verify completion and content hash. Optional: assert `vm_stat` (macOS) / `/proc/vmstat` (Linux) page-cache deltas; flaky in CI, so keep it manual.
- Benchmark in PR description: same template as plan 01.

## Risk and rollback

Risk: low to medium.

- Wrong hints can hurt small workloads (e.g. SEQUENTIAL hint on a tiny file, DONT_NEED on a file we'll re-read). Mitigations: pick conservative thresholds, don't hint for files below 1 MiB, validate with a benchmark.
- `F_NOCACHE` for write hurts if a downstream consumer immediately reads the file. The threshold (64 MiB) is well above typical re-read patterns; readers of large outputs (e.g. cp into chroot) re-fault from disk anyway.

Rollback: each commit is independently revertable.

## Dependencies

- Independent of plans 01 / 02 / 03. Soft-shares the `darwin-*-threshold` setting concept with plan 02; if 02 lands first, plan 04 reuses or extends the same setting.

## Definition of done

- [ ] Unit + functional tests green.
- [ ] Benchmark numbers in PR description.
- [ ] Release note merged.
- [ ] Threshold settings documented.
