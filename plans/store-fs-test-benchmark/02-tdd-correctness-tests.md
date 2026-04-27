# 02 — TDD workflow for correctness tests

## Goal

Standardise the "test-first" pattern that every FS optimisation plan
follows, so that:

- Each new helper (`disableCoW`, `cloneFile`, `hintSequential`,
  `createBtrfsSubvolume`, `writeIncompressibleBytes`, …) ships with
  its unit test in the same commit, written *before* the
  implementation in PR review terms.
- Each modified call site (`recursiveSync`, `optimisePath_`,
  `moveOutputToTempDir`, GC-reserve open, SQLite open, NAR restore,
  `--repair-path` read) ships with a functional or NixOS test that
  fails on `master` and passes on the feature branch.
- The "before / after" diff in the PR description is reproducible by
  any reviewer with one command.

## Why

The existing FS-optimisation plans each list "Tests:" sections with
varying granularity. Some say "unit + functional"; some say "extend
existing test"; some say "manual on real volume". This plan replaces
those scattered prescriptions with a uniform recipe: every plan ends
up with a per-helper unit test, a per-call-site functional or NixOS
test, and where applicable a `_NIX_TEST_ACCEPT=1`-friendly golden
fixture.

The recipe also makes review easier: a reviewer reading a plan-NN PR
sees the test commit first, the implementation commit second, and can
verify the test fails on `master`.

## Existing related upstream work

- `doc/manual/source/development/testing.md` — Nix's existing testing
  guide. This plan extends it with an FS-optimisation-specific recipe.
- `_NIX_TEST_ACCEPT=1` mechanism — used by language tests and
  characterisation tests. This plan brings it into FS-test scope.
- No PRs.

## Files touched

- `doc/manual/source/development/testing.md` — new section "Test-first
  for filesystem optimisations" with the recipe below.
- `tests/perf/` — new top-level test tree that runs under
  `meson test --suite perf` (no perf yet — that's plans 03/04 — but
  the directory + meson hookup land here so 03/04 are additive).
- `src/libutil-tests/cow-helpers.cc` — example unit test for the
  cross-cutting helpers (`cloneFile`, `disableCoW`,
  `writeIncompressibleBytes`, cache hints) that several plans share.
  Lands as a stub with `GTEST_SKIP()` until plan 04's `cloneFile`
  helper exists; the stub reserves the test-class name.
- One commit that goes into each FS-optimisation plan as commit 0:
  "tests: add failing unit/functional test for plan-NN" — referenced
  here as a recipe, not as a file change in this plan.

## Recipe

For every FS-optimisation plan with a new helper or modified
call site:

1. **Commit 0 (test-first)** — add the test that *will* pass after the
   feature lands. Mark it `GTEST_SKIP()` (unit) or `skip "feature not
   yet wired"` (functional/NixOS) so CI stays green on the
   intermediate commits. The skip-message must reference the plan
   number.

2. **Commits 1..N-1 (implementation)** — land the helper(s) and call
   sites. Each helper commit removes its skip-marker.

3. **Commit N (golden fixtures, if any)** — for changes that affect
   serialised output (NAR with `~nix~case~hack~` suffix, FOD outputs
   with mode bits), land the regenerated golden fixture in a final
   commit, with `_NIX_TEST_ACCEPT=1` in the commit body so the
   regeneration is reproducible.

This structure means a reviewer can:

- `git checkout HEAD~N && meson test -C build <name>` — see the test
  pass with `SKIPPED`.
- `git checkout HEAD && meson test -C build <name>` — see the test
  pass for real.
- `git checkout HEAD~N -- src/ && meson test -C build <name>` — see
  the test fail (test-first proof).

## Test taxonomy

Per the existing structure under `src/*-tests/` and `tests/`:

| Layer | Where | When | What |
| --- | --- | --- | --- |
| Unit | `src/libutil-tests/`, `src/libstore-tests/` | Per helper | gtest+rapidcheck. One file per helper. Use `tmpDir` from existing fixtures. |
| Functional | `tests/functional/<name>.sh` | Per Nix command flow | bash; uses `lib/scratch-volume.sh` from plan 01 when FS-aware. Add to the appropriate suite (`ca`, `flakes`, `lang`, …). |
| NixOS | `tests/nixos/<name>.nix` | Per FS-aware end-to-end flow | uses `lib/fs-scratch-volume.nix` from plan 01. Required when the test needs an actual btrfs/xfs/zfs/APFS-case-sensitive volume. |
| Characterisation | `_NIX_TEST_ACCEPT=1` | Per serialised-output change | regenerate the golden file. |

Each FS-optimisation plan picks the layer(s) it needs from this table
and writes commit 0 accordingly.

## Commits (in order)

1. **`doc: add "test-first for filesystem optimisations" section to testing.md`**
   - Captures the recipe above.
   - References plan 01's fixture, plan 03/04's bench harness.

2. **`tests/perf: add empty perf-suite scaffold`**
   - `tests/perf/meson.build` registering an empty `perf` suite.
   - One placeholder test (`tests/perf/placeholder.sh` exits 0 with a
     "no perf tests yet" message) so the suite is runnable from day
     one. Plan 03 fills it in.

3. **`src/libutil-tests: add cow-helpers stub test class`**
   - `src/libutil-tests/cow-helpers.cc` with one `GTEST_SKIP()` per
     planned helper (`cloneFile`, `disableCoW`,
     `writeIncompressibleBytes`, `hintSequential`, `hintDontNeed`).
   - Each skip-message references the FS plan that will fill it in.
   - This commit makes the test-first recipe concrete: future helper
     PRs delete the `GTEST_SKIP()` line and add the body.

4. **`docs: cross-reference each FS-optimisation plan to this plan`**
   - Sweep every `plans/store-*-optimisations/*.md` file. Replace the
     inline "Tests:" prescriptions with a one-liner pointing at this
     plan's recipe.
   - Keep each plan's "Definition of done" intact — those are
     plan-specific assertions.

## Tests

This plan is itself test infrastructure. The "tests of the test
infrastructure" are:

- Commit 2's perf-suite scaffold runs and reports "skipped" on every
  CI runner.
- Commit 3's stub test class runs and reports `SKIPPED` for each
  helper, with a reference to its plan number visible in the test
  output.

## Risk and rollback

Risk: minimal. This plan adds organisational structure, no behaviour
change.

- Risk: the recipe could be ignored by future PRs. Mitigation: the
  PR-template should ask "does this PR add a helper or call-site
  modification covered by `plans/store-fs-test-benchmark/02`?" — small
  CONTRIBUTING.md update.

Rollback: revert any subset; the recipe itself is documentation.

## Dependencies

- **Hard:** plan 01 (the scratch-volume rig). Without it, the
  recipe's "NixOS layer" prescription is unfulfilled.
- Independent of plans 03 / 04.
- The FS-optimisation plans don't strictly *depend* on this plan
  before they can ship, but plans that ship without following the
  recipe will require a manual cleanup pass when plan 04's CI
  regression detection lands.

## Definition of done

- [ ] Testing-doc section landed.
- [ ] Empty perf suite scaffold lands and is green.
- [ ] `cow-helpers` stub test class lands; each FS plan that adds a
      helper deletes its skip and supplies a body.
- [ ] Every FS-optimisation plan in `../store-apfs-optimisations/`
      and `../store-btrfs-zfs-optimisations/` cross-references this
      plan.
- [ ] CONTRIBUTING.md mentions the recipe.
