# Store APFS / Darwin optimisations — implementation plan

This folder contains six self-contained implementation plans, ordered by independence (lowest-risk first → most invasive last). Each plan targets one optimisation, lists the commits it should produce, the tests that gate it, and its dependencies on other plans and on upstream in-flight work.

| # | File | Title | Risk | Self-contained? | Blocked by |
|---|---|---|---|---|---|
| 01 | [`01-f-barriersync.md`](01-f-barriersync.md) | Switch macOS `syncDescriptor` from `F_FULLFSYNC` to `F_BARRIERFSYNC` for store-path data | low | yes | — |
| 02 | [`02-darwin-startfsync.md`](02-darwin-startfsync.md) | Make `AutoCloseFD::startFsync()` do useful work on Darwin | low | yes | — |
| 03 | [`03-runtime-case-sensitivity.md`](03-runtime-case-sensitivity.md) | Detect case-sensitive APFS at runtime instead of forcing `use-case-hack` on for all of Darwin | low | yes | — |
| 04 | [`04-cache-hints.md`](04-cache-hints.md) | Add `posix_fadvise` / `F_NOCACHE` / `F_RDADVISE` hints to bulk passes (`recursiveSync`, NAR streaming, large `RestoreRegularFile` writes) | medium | yes | — |
| 05 | [`05-copyfile-clone.md`](05-copyfile-clone.md) | Use Darwin `copyfile(3)` with `COPYFILE_CLONE` (and Linux `FICLONE` ioctl) inside `copyFile()` and `copyRecursive()` | medium | yes | soft-depends on 01 (shares the Darwin probe pattern) |
| 06 | [`06-clonefile-dedup.md`](06-clonefile-dedup.md) | Opt-in `clone-store-paths` mode for `optimise-store` using APFS `clonefile(2)` (and Linux `FICLONERANGE`) instead of hardlinks | high | rebases | **PR #15460** must merge or be abandoned first; coordinate with **PR #15204** (BLAKE3 farm) and **issue #9450** (incremental optimisation) |

## Dependency graph

```
01 ──┐
02 ──┼──── all four ship independently against master
03 ──┤
04 ──┘
              │
              ▼
05 (uses the Darwin probe pattern from 01; touches src/libutil/file-system.cc)
              │
              ▼
06 (rebase on PR #15460 once it lands; new `clone-store-paths` setting; opt-in)
```

Plans 01–04 can ship in any order, in parallel branches. Plan 05 reads better after 01 has established the macOS-version probe convention. Plan 06 is intentionally last because (a) it touches files in active conflict with at least two open PRs, (b) it changes the GC contract (clones don't share an inode, so `nlink == 1` reachability stops applying), and (c) it benefits from the inline-hashing infrastructure in #15460.

## Commit style

Each plan lists discrete commits. Rules common to all of them:

- One logical change per commit, in the order listed inside the plan; commits build & test independently.
- Subject prefix follows current repo convention from `git log`: `libstore:`, `libutil:`, `tests:`, `doc:`, `installer:`.
- Body explains **why**; the "what" is in the diff.
- New user-visible setting? add it to `local-settings.hh` *and* a `doc/manual/rl-next/<topic>.md` release note in the same commit.
- New macOS syscall? wrap in an availability check (`__builtin_available` or feature-test macro) and provide a runtime fallback for the build's deployment target — the upstream CI builds on macOS 13+, but Nixpkgs ships `aarch64-darwin` Nix back to macOS 11 and `x86_64-darwin` back to macOS 10.13. See `flake.nix` for the supported systems list.
- Default behaviour unchanged unless the plan explicitly says otherwise (most of these are opt-in or behind a runtime probe).

## Testing strategy common to all plans

- **Unit tests:** add to `src/libutil-tests/` or `src/libstore-tests/` next to the changed file. macOS-only paths must compile-time-skip on Linux.
- **Functional tests:** prefer adding to `tests/functional/optimise-store.sh` or a new `tests/functional/<feature>.sh` listed in `tests/functional/meson.build`. Tag Darwin-only tests with the existing `requireDaemonNewerThan` / system filter pattern.
- **NixOS VM tests:** none of these need them — APFS isn't reachable from VM tests. Reviewers will ask; cite this README.
- **Benchmarks:** when claiming a perf win, follow the exact format Mic92 used in PR #15460 (paths count, total bytes, n iterations, p-value, side-by-side timings). Drop the numbers into the PR description.

## Coordination with in-flight upstream work

| PR / Issue | What it does | Interaction |
|---|---|---|
| [PR #15460](https://github.com/NixOS/nix/pull/15460) `optimize-store` (Mic92) | Inline-hash during `restorePath`; passes precomputed hashes to `optimisePath`. Touches `optimise-store.cc`, `local-store.{cc,hh}`. Base branch `fast-nix-copy`. | **Plan 06 must rebase on this.** When it lands, the `inlineHashes` parameter becomes the natural place to ask "should we clone instead of link?". |
| [PR #15204](https://github.com/NixOS/nix/pull/15204) `.links-b3/` BLAKE3 farm (edef) | Adds an experimental `blake3-links` farm parallel to `.links/`. | Plan 06 chooses cloning vs linking; orthogonal to choosing SHA-256 vs BLAKE3. The two should compose: a `clone-store-paths` user could pick either farm style. Resolve merge conflicts in `optimise-store.cc` after both land. |
| [Issue #14599](https://github.com/NixOS/nix/issues/14599) (Dec 2025) | Store optimisation occasionally hardlinks non-identical files (suspected stat-cache TOCTOU). | **Plan 06 sidesteps this class of bug** because clones don't share an inode — a stale stat won't propagate corruption. Worth mentioning in the PR. |
| [Issue #9450](https://github.com/NixOS/nix/issues/9450) (Apr 2026) | Incremental optimisation via an `optimised` column in `ValidPaths`. | Orthogonal to plan 06 (cloning is *how*, incremental is *which paths*). Don't claim plan 06 closes it. |
| [Issue #10746](https://github.com/NixOS/nix/issues/10746) (Sep 2025) | Case-sensitive APFS installer + ecosystem. | Plan 03 is the runtime-probe half of this; the installer half is out of scope and noted in plan 03. |
| Commit `52011de1b` (Apr 21 2026) | Already replaced FOD-output NAR round-trip with `copyRecursive`, leaving the explicit `reflink` TODO at `derivation-builder.cc:1766`. | Plan 05 is the natural follow-up; cite this commit in the PR. |

Re-check this table before opening any PR — these threads move weekly.
