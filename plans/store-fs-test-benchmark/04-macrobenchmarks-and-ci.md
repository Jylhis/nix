# 04 — Macrobenchmarks + CI regression detection

## Goal

End-to-end timed benchmarks of full Nix operations
(`nix copy`, `nix build`, `nix-store --optimise`,
`nix-store --gc`, `nix-store --repair-path`,
NAR restore of large derivations) on each supported filesystem,
plus a Hydra job that publishes baseline JSON and a per-PR CI job
that compares and fails on regressions exceeding configurable
thresholds.

## Why

Microbenchmarks (plan 03) tell you whether `FICLONE` is faster than
`read+write` for a single 16 MiB file. They don't tell you whether
`nix-store --optimise` on a 50 GB store is faster end-to-end after
all the related changes land.

Macrobenchmarks measure the user-visible quantity. They're the
numbers that should appear in PR descriptions and that should drive
the "did this perf-targeting PR actually help?" decision.

CI regression detection closes the loop: once a PR claiming "X% faster"
lands, no future PR can silently undo the gain. The CI job:

1. Builds the tree at the PR's commit.
2. Runs the macrobench suite against a baseline saved by Hydra from
   the most recent `master` build.
3. Computes per-bench deltas. If any bench is >10% slower (configurable),
   marks the run "regression" and fails the check.

## Existing related upstream work

- Nix has no perf-CI today. The `tests/nixos` matrix is correctness-only.
- PR #14991 (process-perf) ships ad-hoc benchmarks in PR descriptions;
  this plan would standardise that.
- Hydra has the build infrastructure (per-system runners) but no
  per-PR perf comparison job.

## Files touched

- `tests/perf/operations/` — new bench tree.
- `tests/perf/operations/nix-copy.sh` — `nix copy` to scratch FS;
  reports time, peak RSS, bytes-written.
- `tests/perf/operations/nix-build.sh` — `nix build` of a fixed
  derivation (10 k-file kernel-config-style; pinned in
  `tests/perf/operations/fixtures/`).
- `tests/perf/operations/nix-store-optimise.sh` — preload N store
  paths, run `--optimise`, time it.
- `tests/perf/operations/nix-store-gc.sh` — preload + delete N store
  paths, run `--gc`, time it.
- `tests/perf/operations/nar-restore.sh` — large derivation NAR
  restore.
- `tests/perf/operations/lib.sh` — shared helpers
  (warm-up, JSON output, geomean across N iterations).
- `tests/perf/baseline/<arch>-<fs>.json` — Hydra-published baselines
  per (arch, FS) tuple. Updated by a Hydra cron job every time
  `master` advances.
- `flake.nix` — register `hydraJobs.perf.baseline.<arch>-<fs>` and
  `hydraJobs.perf.compare-pr.<arch>-<fs>` jobs.
- `.github/workflows/perf-comparison.yml` — per-PR GitHub Actions job
  that downloads the baseline, runs the local macrobench, compares,
  comments on the PR.
- `tests/perf/compare.py` — JSON diff tool with configurable
  thresholds; outputs a markdown comment for the PR.
- `doc/manual/source/development/testing.md` — extend with
  "Performance regression CI" section.

## Macrobench suite

Each macrobench runs through `lib.sh` which provides:

- **N-iteration warm-up + measurement** — default 1 warm-up + 5
  measurement; geomean reported.
- **Cache-clear hooks** — per-FS scratch volume from plan 01,
  unmount/remount between iterations to drop OS caches reliably.
- **JSON output** — same schema as plan 03's microbench output, with
  one row per (operation, fs, iteration).
- **`taskset` / `nice -n 0`** — pin to a stable CPU set when root.
- **Failure modes**: if the operation itself fails, mark the bench
  failed and abort the whole macrobench (don't compare timings of a
  broken Nix).

## Per-bench specs

| Bench | What | Expected sensitivity |
| --- | --- | --- |
| `nix-copy` | `nix copy /nix/store/...-firefox --to ./scratch` (cold cache) | Plans 04/05/06 (APFS), 04/06 (btrfs/zfs); copy_file_range / FICLONE / clonefile |
| `nix-build` | A fixed `kernelConfig` derivation that produces ~75 k files | Plans 02 (Darwin startFsync), 04 (cache hints), 05 (subvolume) |
| `nix-store-optimise` | Auto-optimise a preloaded ~5 GB store | Plans 06 (APFS clonefile), 04 (Linux FICLONE), `optimize-store` PR #15460 |
| `nix-store-gc` | GC of preloaded ~5 GB store | Plans 06 (APFS GC), 02 (Linux GC reserve) |
| `nix-store-repair-path` | Pre-corrupted store path | Plan 07 (ZFS EIO repair) |
| `nar-restore-large` | Restore a 1 GB random-bytes NAR | Plan 02 (Darwin startFsync), 04 (cache hints) |
| `sqlite-write-pattern` | 10 k inserts into the Nix SQLite db | Plan 01 (`chattr +C` btrfs) |
| `gc-reserve-realloc` | Trigger reserve recreate on btrfs+zstd | Plan 02 (incompressible reserve) |

Each bench specifies which plans it's sensitive to, so a PR's CI
output can flag "this PR claims to land plan 06; the relevant
macrobench delta is 22% — green".

## Commits (in order)

1. **`tests/perf/operations: add lib.sh + nix-copy.sh`**
   - First macrobench. Establishes the JSON output format (matches
     plan 03's schema), the warm-up methodology, and the
     scratch-volume integration.
   - Lands with a placeholder `tests/perf/baseline/x86_64-linux-ext4.json`
     captured manually so the comparison flow can be tested.

2. **`tests/perf/operations: add nix-build, nix-store-optimise, nix-store-gc`**
   - Three more benches following the lib.sh pattern.
   - Pins the build fixture (a derivation with predictable output
     count) to `tests/perf/operations/fixtures/`.

3. **`tests/perf/operations: add NAR restore, repair-path, sqlite-write benches`**
   - Final three benches.

4. **`tests/perf: add compare.py`**
   - Reads two JSON files (baseline + current), computes per-bench
     deltas, applies thresholds (default ±10% wall-clock, ±20% RSS),
     emits markdown summary suitable for a PR comment.
   - `--bench-sensitivity-map` accepts a YAML mapping of bench →
     plan numbers; output annotates each delta with relevant plans.

5. **`flake: add hydraJobs.perf.{baseline,compare-pr}.<arch>-<fs>`**
   - Per (arch, fs) tuples: `aarch64-linux × {ext4, btrfs, xfs, zfs}`,
     `x86_64-linux × {ext4, btrfs, xfs, zfs}`,
     `aarch64-darwin × {apfs-case-insensitive, apfs-case-sensitive}`.
   - Baseline jobs run on `master` and publish JSON to a known
     artefact location.
   - Compare-pr jobs run on PR commits; download the latest
     baseline, run the bench, run `compare.py`, post a comment.

6. **`ci: add perf-comparison GitHub Actions workflow`**
   - Triggered on PR push.
   - Calls into the Hydra `compare-pr` job, downloads its output,
     posts/updates a comment titled "Perf delta vs `master` baseline"
     with the markdown table.
   - Marks the check failed if any bench delta exceeds the threshold
     **and** the PR doesn't include `perf-regression-acknowledged:
     <plan-NN>` in its body.

7. **`doc: document the perf-CI flow`**
   - In `doc/manual/source/development/testing.md`, add "Performance
     regression CI" with: how to read the PR comment, how to acknowledge
     an intentional regression, how to add a new bench, how to update
     the baseline (manual override via `update-baseline` label).

## Methodology

- **Variance budget**: aim for ≤3% run-to-run noise on the geomean
  of 5 iterations. If a bench is noisier, mark it "noisy" in the
  output and exclude from threshold checks (regression detection
  requires reliable signal).
- **Threshold tuning**: start at ±10% wall-clock, ±20% RSS. Tune
  after a month of CI data.
- **Baseline staleness**: baseline regenerates on every `master`
  push that touches `src/libstore/**` or `src/libutil/**` or any
  bench file. Other commits don't trigger baseline regen.

## Tests

Each macrobench's correctness is asserted before timing — see plan 03's
"bench files double as correctness tests" pattern.

`compare.py` has unit tests under `tests/perf/test-compare.py`:
synthetic baseline + current JSONs covering "no regression",
"regression in one bench", "noisy bench excluded", "new bench in
current not in baseline".

## Risk and rollback

Risk: medium.

- **Hydra runner cost**: 8+ (arch × FS) tuples each running 7+ benches
  is non-trivial CI time. Mitigation: only re-run on labels-or-touch,
  not every PR commit. Dropdown: only the `(default-arch, ext4)` tuple
  runs on every PR; full matrix on label.
- **Flaky benches**: macrobenches on shared CI runners are inherently
  noisier than dedicated hardware. Mitigation: variance budget +
  noisy-bench exclusion + ability to acknowledge.
- **Baseline staleness during big refactors**: a refactor PR may cause
  unrelated benches to swing. Mitigation: ack mechanism + per-bench
  threshold override.

Rollback: each commit is independently revertable. The CI workflow
can be disabled via `.github/workflows/perf-comparison.yml` deletion
without affecting any other tests.

## Dependencies

- **Hard:** plan 01 (scratch volumes) and plan 03 (microbenchmark
  schema). Macrobenches reuse the JSON output format.
- **Soft:** plan 02 (TDD recipe). Macrobench updates accompany every
  FS-opt plan that affects an operation in the suite — recipe
  documents the convention.
- Independent of the FS-optimisation plans (this plan ships first;
  the FS-opt plans then update the relevant baseline+threshold).

## Definition of done

- [ ] All seven macrobenches running locally on Linux + macOS dev
      shells.
- [ ] Per-(arch, fs) baseline jobs registered on Hydra and producing
      JSON.
- [ ] Per-PR comparison job posts a markdown comment within 30
      minutes of PR push.
- [ ] `compare.py` unit tests green.
- [ ] Manual section added.
- [ ] Two-week soak: every `master` baseline regenerates without
      flakes; per-PR comparison comments are accurate.
- [ ] At least one FS-optimisation plan has shipped end-to-end
      using this CI to validate its perf claim.
