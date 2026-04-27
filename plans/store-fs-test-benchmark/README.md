# Test and benchmark harness for filesystem-store optimisations

Cross-cutting plan that supports both `../store-apfs-optimisations/` and
`../store-btrfs-zfs-optimisations/` (and any future filesystem
investigations: XFS, ext4, ReFS, …). The goal is two-fold:

1. **TDD enablement** — every optimisation plan in the FS folders should
   land its test before its implementation, in a way that makes the
   "before" measurement (fail / slow) and the "after" measurement
   (pass / fast) trivially diff-able in the PR.
2. **Performance regression detection** — the same harness produces
   stable enough numbers that a PR claiming "20% faster auto-optimise"
   can be verified by a reviewer running one command, and that future
   PRs can't silently regress the gain.

This is **not** about adding more tests for the sake of coverage. It's
infrastructure that the per-feature plans depend on; without it each
plan would re-invent the same scratch-volume + timing rig.

## Why a separate plan instead of folding into each feature plan

Each FS plan currently says "add a NixOS test that mounts a btrfs/APFS
scratch volume". That's six plans inventing the same rig in subtly
different ways. Pull the rig out, get one well-tested implementation,
and let each feature plan call into it with two lines.

Same for benchmarks: each FS plan says "benchmark in PR description".
Without a shared baseline + variance methodology, those numbers aren't
comparable. A shared harness fixes that.

## Plans

| # | Plan | What | When written |
| --- | --- | --- | --- |
| 01 | [FS scratch-volume rig](01-fs-scratch-volume-rig.md) | NixOS test framework that mounts btrfs/xfs/ext4/zfs/tmpfs in a VM, and macOS-runner helpers that probe APFS case-sensitivity. Used by all correctness *and* benchmark tests. | Before any FS plan ships its NixOS test. |
| 02 | [TDD correctness test workflow](02-tdd-correctness-tests.md) | Per-helper unit tests (gtest+rapidcheck), per-call-site functional tests (bash), per-flow NixOS tests. The "test-first" recipe each FS plan follows. | In parallel with the helpers themselves. |
| 03 | [Microbenchmarks](03-microbenchmarks.md) | Per-syscall benchmark suite: `fsync` vs `F_BARRIERFSYNC` vs `F_FULLFSYNC`; `link` vs `clonefile` vs `FICLONE`; `copy_file_range` vs read/write loop. C++ Google Benchmark; reproducible timings; PR-attachable JSON. | Before the related helper PRs to capture the "before" baseline. |
| 04 | [Macrobenchmarks + CI regression detection](04-macrobenchmarks-and-ci.md) | End-to-end Nix operations (`nix copy`, `nix build`, `nix-store --optimise`) timed via a Hydra job that publishes baseline JSON; per-PR job that compares and fails on regressions > N%. | After plan 03 establishes the JSON schema. |

## Dependency graph

```
01 (scratch rigs)        ── prerequisite for 02, 03, 04 NixOS coverage
02 (TDD correctness)     ── per-feature; each FS plan opts in
03 (microbenchmarks)     ── builds on 01; depends on Google Benchmark in deps
04 (macro + CI)          ── builds on 01 + 03; biggest plan; ship last
```

## Coordination with existing FS plans

Every existing plan in `../store-apfs-optimisations/` and
`../store-btrfs-zfs-optimisations/` will be updated to:

1. Replace its inline "NixOS test" section with `see plan 01 + 02`.
2. Replace its inline "benchmark" section with `see plan 03 / 04`.

These updates are part of *this* plan's commit 4 (one cleanup commit
that touches every FS plan to point at the shared infrastructure).

## What this plan does **not** do

- It doesn't add new test types (e.g. fuzz testing, property-based
  testing of the store API). Those are out of scope.
- It doesn't change Nix's existing test suite layout. The new harness
  is additive: a `tests/perf/` tree alongside `tests/functional/` and
  `tests/nixos/`, plus a `src/libutil-bench/` and `src/libstore-bench/`
  alongside the `-tests` siblings.
- It doesn't measure end-to-end Nix expression-evaluation perf or
  flake-cache perf. Those are different systems and want different
  benchmarks; cite as out of scope and link to existing eval-perf
  efforts (e.g. PR #14991 work).

## Reusability notes

The same rig will be reused for future filesystem passes (ext4-only,
XFS-only, F2FS, exFAT, NFS, etc.). The "what's the FS" axis is a
parameter; each new FS gets one row in plan 01's mount-helper table.

The Google Benchmark microbench harness is also useful for non-FS
hot paths — eval, parser, hashing — so plan 03 deliberately keeps the
pattern generic. Existing eval-perf efforts (PR #11130 `GC_INITIAL_HEAP_SIZE`,
PR #14991 process-perf) can adopt the same infrastructure.
