# 03 — Microbenchmark suite for FS primitives

## Goal

A Google Benchmark–based microbench suite that times each
filesystem primitive Nix uses or wants to use, on each filesystem
under `tests/perf/fs-primitives/`. Output is JSON consumed by plan 04's
CI regression detector. The suite is runnable locally with one
command and produces stable enough numbers (warm-up + 5 iterations +
geomean) that PRs can attach the JSON.

## Why

Today every FS-optimisation plan says "benchmark in PR description".
There's no shared definition of what to measure, no shared output
format, no shared methodology. Two plans claiming "30% faster" might
mean wildly different things.

The microbench suite makes per-primitive timing reproducible:

| Primitive | Baseline | Variants we want to compare |
| --- | --- | --- |
| `fsync` | plain `fsync` | `F_FULLFSYNC`, `F_BARRIERFSYNC`, `sync_file_range`, `aio_fsync` |
| File copy | `read+write` loop | `copy_file_range`, `FICLONE`, `clonefile`, `copyfile(COPYFILE_CLONE)` |
| Hardlink vs clone | `link(2)` | `clonefile(2)`, `FICLONE` |
| Page-cache hint | unhinted read | `posix_fadvise(SEQUENTIAL)`, `F_RDAHEAD` |
| Page-cache drop | unhinted | `posix_fadvise(DONTNEED)`, `F_NOCACHE` |
| GC reserve | `posix_fallocate` + zero-fill | counter-pattern, `disableCoW` + zero-fill |

Each row is one bench file. Each FS gets its own runner. Comparison
across FSes and across variants becomes a JSON post-processing
exercise instead of a manual benchmark dance.

## Existing related upstream work

- No microbench suite exists in NixOS/nix today.
- PR #11130 mentions GC heap-size benchmarking but uses ad-hoc shell
  loops; this plan provides a more structured replacement.
- PR #14991 "Process-performance improvements" (Connor Baker, open)
  could adopt the same harness for non-FS perf.
- Lix has `lix-bench` for evaluator benchmarks; no FS bench.

## Files touched

- `src/libutil-bench/` — new sibling to `src/libutil-tests/`.
- `src/libstore-bench/` — new sibling to `src/libstore-tests/`.
- `src/libutil-bench/fs-primitives.cc` — bench cases for `fsync`,
  `copy_file_range`, `FICLONE`, `clonefile`, cache hints,
  `posix_fallocate`.
- `src/libutil-bench/meson.build` — links Google Benchmark, registers
  the bench under `meson test --benchmark`.
- `nix-meson-build-support/deps-lists/meson.build` — add
  `dependency('benchmark')` (Google Benchmark) as an optional dep.
- `flake.nix` / `package.nix` — wire the dep through the dev shell.
- `tests/perf/fs-primitives/run.sh` — wrapper that emits the
  benchmark JSON via Google Benchmark's `--benchmark_format=json`.
- `tests/perf/fs-primitives/README.md` — how to run and read the
  output.
- `doc/manual/source/development/testing.md` — extend the testing
  doc's perf section.

## Bench JSON schema

Google Benchmark's native JSON, augmented with a `nix-meta` block:

```json
{
  "context": { "...": "..." },
  "nix-meta": {
    "fs": "btrfs",
    "fs-mount-opts": ["compress=zstd:3"],
    "kernel": "6.6.15",
    "nix-commit": "abcdef0",
    "machine": "ci-aarch64-linux-1"
  },
  "benchmarks": [
    {
      "name": "BM_Fsync_FullFsync",
      "iterations": 1000,
      "real_time": 12345.6,
      "cpu_time": 234.5,
      "time_unit": "ns",
      "nix-aux": { "bytes-written": 65536 }
    }
  ]
}
```

Plan 04's CI regression detector consumes this format.

## Commits (in order)

1. **`build: optionally depend on Google Benchmark`**
   - Add `benchmark = dependency('benchmark', required: false)` in
     the meson dep block. Make it opt-in via `-Dbench=enabled` so
     non-bench builds don't grow the deps.
   - Add `pkgs.gbenchmark` to the dev-shell when bench is enabled.

2. **`src/libutil-bench: scaffold (empty bench, links Google Benchmark)`**
   - One trivial benchmark (`BM_Memcpy`) to prove the wiring.
   - Registers `meson.add_install_script` etc. so
     `meson test --benchmark` discovers it.

3. **`src/libutil-bench/fs-primitives: fsync variants bench`**
   - `BM_Fsync_Plain`, `BM_Fsync_FullFsync` (`__APPLE__`),
     `BM_Fsync_BarrierFsync` (`__APPLE__`), `BM_Fsync_SyncFileRange`
     (`__linux__`), `BM_Fsync_AioFsync` (`__APPLE__`).
   - Each opens a tmpfile, writes 64 KiB, calls the variant, deletes.
   - The bench accepts `--nix_fs_mountpoint=/mnt/scratch-btrfs` so it
     can be pointed at a per-FS scratch volume from plan 01.

4. **`src/libutil-bench/fs-primitives: copy/clone variants bench`**
   - `BM_Copy_ReadWrite`, `BM_Copy_CopyFileRange` (Linux),
     `BM_Copy_Ficlone` (Linux), `BM_Copy_Clonefile` (Darwin),
     `BM_Copy_CopyfileClone` (Darwin).
   - Each uses a 16 MiB random source file, copies/clones to a fresh
     dest in the scratch dir, deletes both. Reports `bytes-written`
     in `nix-aux`.

5. **`src/libutil-bench/fs-primitives: hardlink vs clone bench`**
   - `BM_Link_Plain`, `BM_Link_Clonefile`, `BM_Link_Ficlone`. 4 KiB
     source file; loop create-link/clone, unlink. Reports inode
     accounting deltas in `nix-aux`.

6. **`src/libutil-bench/fs-primitives: cache hint bench`**
   - `BM_ReadCold`, `BM_ReadCold_FadviseSequential`,
     `BM_ReadCold_Rdahead`. Drops page cache before each iteration
     via `posix_fadvise(DONTNEED)` on warm-up.

7. **`tests/perf/fs-primitives: run.sh wrapper + README`**
   - `run.sh --fs btrfs --json out.json` invokes the bench binary
     against the named FS scratch dir (assumes plan 01's
     `NIX_TEST_SCRATCH_DIR_BTRFS` env var or runs `mkfs+mount` if
     `--mount` is passed and we're root).
   - README documents how to interpret the output and how to add new
     benches.

8. **`doc: testing.md — perf section`**
   - Extends the perf section with: how to enable, how to run, what
     the JSON schema means, and pointers to plans 04 (CI) and the
     per-FS plans (consumers).

## Methodology

To get stable numbers:

- 5-second warm-up per benchmark (Google Benchmark default sufficient).
- Discard outliers >2σ.
- Pin to one CPU via `taskset` or `pthread_setaffinity_np` (the
  wrapper does this when run as root).
- Drop page cache between iterations for cold-cache benches via
  `echo 3 > /proc/sys/vm/drop_caches` (Linux) or `purge` (macOS,
  requires root).
- Report geometric mean across N runs in `run.sh`.

## Tests

Bench files double as their own correctness tests: they always assert
the operation succeeded (`ASSERT_EQ(written, expected)`) before
recording the timing. So a regression that breaks the operation also
breaks the bench, which is caught by `meson test --benchmark`.

## Risk and rollback

Risk: low.

- Adding Google Benchmark as a dep grows closure size for dev shells.
  Mitigation: opt-in (`-Dbench=enabled`); not on by default.
- Microbench numbers vary across CI runners. Mitigation: plan 04's CI
  job pins to specific runner classes (Hydra `aarch64-linux` and
  `x86_64-linux` only).

Rollback: revert per commit; the dep is opt-in and the bench files
sit in their own directory.

## Dependencies

- **Soft:** plan 01 (scratch-volume rig). Without it, benches run on
  whatever FS the runner's `/tmp` is. With it, benches can target a
  named FS.
- **Hard for plan 04:** plan 04's CI consumer requires this plan's
  JSON output schema to be stable.
- Independent of the FS-optimisation plans; they consume bench
  results but don't depend on the suite landing first. (Recommended:
  land this plan and capture "before" numbers before the FS-opt PRs
  open, so the "after" numbers are directly diff-able.)

## Definition of done

- [ ] Google Benchmark optional dep wired.
- [ ] `src/libutil-bench/fs-primitives.cc` lands with all six bench
      classes (fsync, copy, link/clone, cache-hint, GC-reserve,
      generic).
- [ ] Wrapper `tests/perf/fs-primitives/run.sh` runs locally on
      Linux + macOS dev shells.
- [ ] JSON schema documented and stable.
- [ ] Manual section added.
- [ ] Initial "before" baseline JSON captured for current `master`,
      attached to plan 04's first PR.
