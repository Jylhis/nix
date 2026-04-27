# 02 — Make `AutoCloseFD::startFsync()` useful on Darwin

## Goal

Today `AutoCloseFD::startFsync()` (`src/libutil/file-descriptor.cc:279-287`) is a no-op outside `__linux__`, which means `RestoreRegularFile`'s destructor (`src/libutil/fs-sink.cc:211-216`) does nothing useful when extracting a NAR on macOS. Provide a meaningful Darwin implementation so that the "kick writeback off early" intent actually fires there too.

## Why

`RestoreRegularFile`'s destructor closes the file descriptor; before doing so, if `startFsync` is enabled it asks the kernel to *start* writing the data back to disk so the synchronous fsync that runs later (during store-path registration) finishes faster. On Linux that's `sync_file_range(SYNC_FILE_RANGE_WRITE)`. On macOS the closest primitive is `aio_fsync(O_DSYNC, …)` (POSIX-AIO, fire-and-forget), and for very large files we additionally want to disable the unified buffer cache for the write side via `fcntl(fd, F_NOCACHE, 1)` to avoid evicting useful pages.

This is the fsync analogue to plan 04's read-side cache hints; they share infrastructure but live on different sides of the I/O.

## Existing related upstream work

- None directly. The Linux `sync_file_range` path was added in PR #14606 area work, no Darwin counterpart exists.
- Plan 01 may land the `__APPLE__` runtime-probe pattern that this plan reuses.

## Files touched

- `src/libutil/file-descriptor.cc` — implementation.
- `src/libutil/include/nix/util/file-descriptor.hh` — keep signature, document the new behaviour.
- `src/libutil/unix/file-descriptor.cc` — supplementary Darwin-specific helpers if needed.
- `src/libutil/fs-sink.cc` — opportunistic `F_NOCACHE` for files above a threshold (e.g. 16 MiB), gated by a runtime check.
- `doc/manual/rl-next/darwin-startfsync.md` — release note.

## Commits (in order)

1. **`libutil: implement AutoCloseFD::startFsync on Darwin via aio_fsync`**
   - `#ifdef __APPLE__` branch in `startFsync()` builds a `struct aiocb`, sets `aio_fildes = fd`, calls `aio_fsync(O_DSYNC, &cb)`.
   - Ignore failures (matches the "best-effort" comment already on the Linux branch).
   - Use a thread-local `aiocb` so multiple `RestoreRegularFile`s can race without sharing state. (Or heap-allocate one per call and rely on `aio_return` cleanup — simpler if profiling shows allocation isn't hot.)
   - Build & tests pass; behaviour change is invisible without a workload that actually fsyncs later.

2. **`libutil: add F_NOCACHE hint for large RestoreRegularFile writes`**
   - In `RestoreSink::createRegularFile` add a `preallocateContents` callback that, when `len > kRestoreNoCacheThreshold` (define as 16 MiB; tune later) and we're on Darwin, calls `fcntl(fd, F_NOCACHE, 1)`.
   - The Nix store typically restores files much smaller than the buffer cache budget, so this only kicks in for large blobs (Chromium output, kernel debug info, …).
   - Add the threshold as a hidden setting `darwin-nocache-threshold` in `local-settings.hh` so power users can tune without recompile.

3. **`doc: document Darwin startFsync and F_NOCACHE behaviour`**
   - Release note: what changed, who notices (anyone restoring large NARs on macOS), how to disable (`darwin-nocache-threshold = 0`).

4. **`tests: regression test for startFsync on Darwin`**
   - `src/libutil-tests/unix/file-descriptor.cc` — gated on `__APPLE__`; writes a 1 MB file, calls `startFsync`, asserts no error and no crash.
   - Functional test: extend an existing NAR-restore test (e.g. `tests/functional/add.sh`) to exercise files above and below the threshold.

## Availability

- `aio_fsync` is in macOS POSIX since 10.0 — universally available.
- `F_NOCACHE` is in macOS since 10.0 — universally available.
- No SDK gating needed; just `#ifdef __APPLE__`.

## Tests

- Unit test (commit 4).
- Functional: deliberately install a 100 MB derivation output with `fsync-store-paths = true` and time it before/after.
- Benchmark in PR description, same format as plan 01.

## Risk and rollback

Risk: low.

- `aio_fsync` failures are silent (matching the Linux branch); worst case the optimisation does nothing.
- `F_NOCACHE` could *hurt* if a downstream consumer immediately reads the file (the page won't be cached). Mitigation: the threshold defaults large enough that most reads-after-write are still served from cache, and the setting allows tuning to 0 to disable.

Rollback: revert one or both commits. No on-disk state.

## Dependencies

- None on plan 01 strictly; both touch `file-descriptor.cc` but in different functions. Order them by whoever lands first; the second rebases trivially.

## Definition of done

- [ ] All tests green on Linux and macOS CI.
- [ ] Benchmark numbers in PR description.
- [ ] Release note merged.
- [ ] `darwin-nocache-threshold` setting documented in the manual.
