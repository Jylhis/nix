# 06 — `clonefile(2)`-based store-path deduplication (opt-in)

## Goal

Add a `clone-store-paths` setting that replaces the `link(2)` calls inside `LocalStore::optimisePath_` (`src/libstore/optimise-store.cc:188,237`) with APFS `clonefile(2)` on Darwin and Linux `FICLONE` / `FICLONERANGE` on supporting filesystems. When the setting is on, the `.links/` directory is *not* used; clones are made path-to-path directly because each clone is an independent inode.

This is opt-in, distinct from plan 05 (`copyfile`-with-clone is for files in transit; `clonefile` is for the store-internal dedup pass).

## Why

The current optimisation hits four sharp edges that clones eliminate:

1. **`LINK_MAX` cap** (~32 000 hardlinks per inode). The current code has an explicit `too_many_links` bail-out at `optimise-store.cc:240-247` that empty files in particular routinely hit. Clones aren't capped.
2. **macOS `.app/Contents/` hardlink restriction** (`optimise-store.cc:108-119`) — a 7-year-old workaround that skips dedup entirely inside `.app` bundles. Clones aren't blocked.
3. **Race with writers via shared inode** (`optimise-store.cc:140-143` "skipping suspicious writable file") — the symptom in [issue #14599](https://github.com/NixOS/nix/issues/14599) (Dec 2025) is that store optimisation hardlinked non-identical files due to a stat-cache TOCTOU. With clones each linked file has its own inode, so a stale stat can't propagate corruption to other paths.
4. **Read-only-by-link-count enforcement** — the `MakeReadOnly` dance and the `permissions(…, perm_options::add)` loops exist because hardlinked store paths share the inode, so `chmod` on one affects all. Clones decouple this.

Trade-off: clones don't share an inode, so the GC's `nlink == 1 ⇒ free to delete` short-circuit (`gc.cc:788-789`) doesn't apply. Need a different reachability check: the clone refcount lives at the extent level, not the inode level. APFS exposes this only indirectly (via `getattrlist` `ATTR_FILE_TOTALSIZE` vs `ATTR_FILE_DATALENGTH`), and there's no direct "is this extent shared?" query — so the deletion path falls back to "always safe to delete; the extents persist as long as another clone references them". That actually *simplifies* GC.

## Existing related upstream work

This area is hot. **Coordinate carefully.**

- **[PR #15460](https://github.com/NixOS/nix/pull/15460)** `optimize-store` (Mic92, open Mar 2026; base `fast-nix-copy`). Inline-hashes during `restorePath` and passes precomputed hashes into `optimisePath`. ~18% faster `auto-optimise-store` benchmarked. Touches `optimise-store.cc`, `local-store.cc`, `local-store.hh`. **This plan must rebase on top of it once it lands** — the `inlineHashes` parameter is the natural decision point for "clone vs link".
- **[PR #15204](https://github.com/NixOS/nix/pull/15204)** `.links-b3/` BLAKE3 hardlink farm (edef, open Feb 2026). Adds an experimental parallel farm with type suffixes encoding mode bits. Same area but a **different axis**: that PR picks a hash function and a directory layout; this plan picks a syscall (link vs clone). The two compose cleanly — a `clone-store-paths` user could pick either farm style for indexing — but expect physical merge conflicts in `optimise-store.cc`.
- **[Issue #14599](https://github.com/NixOS/nix/issues/14599)** "Store optimisation hard-links non-identical files" (Dec 2025). This plan **sidesteps** the bug class: stale `st.st_size` can't cause cross-path corruption when each path has its own inode. Mention this in the PR; do not claim the issue is closed (the underlying TOCTOU still wants fixing).
- **[Issue #9450](https://github.com/NixOS/nix/issues/9450)** "Incremental store optimisation" (Apr 2026). Sketches an `optimised` column on `ValidPaths` so re-runs skip already-handled paths. **Orthogonal** to this plan — incremental is *which* paths, cloning is *how*. Be careful not to claim this plan resolves it.
- **[Issue #1272](https://github.com/NixOS/nix/issues/1272)** "Allow some derivations to hardlink to other files in the store" (open since 2017). Different feature, but its discussion thread covers prior community thinking on hardlink semantics; useful background.

No existing work on `clonefile` or `FICLONE` for the dedup pass in NixOS/nix or Lix.

## Files touched

- `src/libstore/optimise-store.cc` — primary change.
- `src/libstore/include/nix/store/local-store.hh` — declare any new helpers.
- `src/libstore/include/nix/store/local-settings.hh` — new setting `clone-store-paths`.
- `src/libstore/gc.cc` — GC short-circuit must not assume `nlink > 1` when clones are in use.
- `src/libutil/include/nix/util/file-system.hh` + `src/libutil/unix/file-system.cc` — `cloneFile(src, dst)` helper returning `bool` (false ⇒ caller must fall back to link/copy).
- `tests/functional/optimise-store.sh` — extend.
- `tests/nixos/auto-optimise-store.nix` — Linux btrfs scratch volume to exercise `FICLONE` if available.
- `doc/manual/source/store/types/local-store.md` — document the setting.
- `doc/manual/rl-next/clone-store-paths.md` — release note.

## Setting design

```
clone-store-paths = false   # default; current behaviour (hardlinks via .links/)
                  | apfs     # use clonefile(2) on Darwin; require it (error if not APFS)
                  | linux    # use FICLONE on Linux; require it (error if not btrfs/xfs/zfs)
                  | auto     # probe per-store and pick clones if supported, else hardlinks
```

Default `false` so users opt in. `auto` is the eventual recommended default once the feature has soaked.

When clones are in use the `.links/` directory is unused for clone-mode paths. Provide a migration command — `nix store optimise --clones` — that walks existing hardlinks and converts them to clones (delete the `.links/` reference, clone the now-orphaned extents to each user). This is *additive*; it doesn't break existing hardlink users.

## Commits (in order)

1. **`libutil: add cloneFile helper`**
   - `bool cloneFile(const path & src, const path & dst)`. Linux: `FICLONE` ioctl. Darwin: `clonefile(2)` with `CLONE_NOFOLLOW`. Else: return false.
   - Returns false (rather than throws) on `EOPNOTSUPP` / `EXDEV` / `EINVAL` so the caller can fall back gracefully.
   - Unit test: clone a temp file, verify content equality and (where possible) shared extents.

2. **`libstore: add clone-store-paths setting (default false, behaviour unchanged)`**
   - Add the enum and parse logic. No call sites use it yet. Documented as experimental.
   - Release note draft (will be polished in a later commit).

3. **`libstore: clone-mode optimisePath_ — direct path-to-path clones`**
   - When `clone-store-paths` is `apfs` / `linux` / `auto-and-supported`:
     - Skip the hash/`.links/` lookup entirely.
     - For each path inside the store directory, walk a per-path content hash → previously-seen path map (in-memory only; no on-disk index needed because clones are independent inodes).
     - When a duplicate hash is found, clone *previous → current* atomically through a temp path (same dance as today, replacing `create_hard_link` with `cloneFile`).
   - Skip-conditions (writable bit, non-regular file types) unchanged.

4. **`libstore: GC awareness for clones`**
   - In `gc.cc:788-789` (`nlink == 1 ⇒ orphan`), short-circuit to "always safe" when the store is in clone mode — `nlink` is irrelevant because each clone has its own inode.
   - Add a code comment pointing at this plan.

5. **`tests: functional coverage of clone-store-paths`**
   - Extend `tests/functional/optimise-store.sh`. Add a per-mode matrix (`hardlink` baseline, `apfs`, `linux`, `auto`).
   - Linux NixOS test: spin up a btrfs scratch volume, set `clone-store-paths = linux`, run a build with auto-optimise, assert disk usage ≈ Σ unique content (not Σ all paths).
   - macOS: gate via `requireDaemonNewerThan` style, run only when APFS detected. CI on `aarch64-darwin` runners satisfies this.

6. **`doc: document clone-store-paths and the GC implication`**
   - Manual page entry under "Local store" types.
   - Release note: experimental, opt-in, what it gives you, what it changes about disk-usage accounting (`du` numbers will look different).

7. **`libstore: nix store optimise --clones migration command`** *(optional follow-up; can defer to a follow-up PR if the main one is too large)*
   - Walk `.links/`, for each entry rewrite the linked store-paths as clones, drop the `.links/` entry.
   - Reversible by running `nix store optimise` again with `clone-store-paths = false`.

## Dependencies

- **Hard:** rebase on PR #15460 once it lands. If #15460 is abandoned or stalls past two months, consider lifting its `inlineHashes` plumbing into this PR.
- **Soft:** PR #15204 in the same area. Resolve textual conflicts when both are in flight.
- **Soft:** plan 01 (`F_BARRIERFSYNC`) — clones still need a barrier-fsync after creation; reuses the helper.
- **Soft:** plan 05 (`copyfile`-with-clone) — different syscall but same `cloneFile` helper from commit 1 here can be shared.
- **Independent:** plans 02, 03, 04.

## Risk and rollback

Risk: high — this is the most invasive of the six.

- GC-contract change is the biggest risk. Mitigations: opt-in setting, exhaustive test matrix, clear release-note callout.
- Disk-usage estimation tools (e.g. `nix path-info -S`, `du`, `ncdu`) will report different numbers. Document.
- Verify behaviour under `nix-store --verify --check-contents` — verification reads the file content; with clones each read goes to the same shared extent, so verification cost is unchanged.
- Verify behaviour under `nix-store --repair` — the repair path currently assumes hardlink semantics in `optimise-store.cc:166-183`; clone mode needs a parallel "delete corrupted clone, re-clone from a known-good source path" branch.

Rollback: each commit is revertable. The setting defaulting to `false` means even partial revert leaves users on the original behaviour. The migration command (commit 7) is reversible via running standard optimise.

## Coordination plan with upstream

1. Subscribe to PR #15460 and PR #15204 notifications. Comment on each linking this plan, asking the authors whether they'd welcome a follow-up that adds clone mode.
2. Open the `cloneFile` helper PR (commits 1 + 5's unit test) **first**, as a no-op-by-default infrastructure change. This lands trivially.
3. Open the dedup PR (commits 2-6) once #15460 has merged; rebase on its `inlineHashes` plumbing.
4. Open the migration command PR (commit 7) last.

## Definition of done

- [ ] All commits land in the order above.
- [ ] Functional + NixOS test matrix green.
- [ ] Manual updated under `store/types/local-store.md`.
- [ ] Release note explicitly flags the GC + `du` accounting differences.
- [ ] Issue #14599 referenced (sidestepped, not closed).
- [ ] Benchmark in PR description: `nix-store --optimise` on a 50 GB store, hardlink baseline vs clone mode, on APFS and on Linux btrfs.
