# 01 — Filesystem scratch-volume rig (NixOS + Darwin)

## Goal

A reusable test fixture that, given an FS name (`btrfs`, `xfs`, `ext4`,
`zfs`, `tmpfs`, `apfs-case-sensitive`, `apfs-case-insensitive`), gives
test code a mounted scratch volume to run against. Used by every
correctness and benchmark test in plans 02-04 and by every per-FS NixOS
test in the existing FS-optimisation plans.

## Why

`tests/nixos/fsync.nix` already does this for ext4/btrfs/xfs in an
ad-hoc way (see `:29-56`). Each new test that wants the same
scratch-volume capability today copy-pastes that boilerplate, makes
small variations, and we end up with subtle drift (different sizes,
different mount options, different post-mount steps).

A library-style fixture eliminates the drift, makes the FS axis a
proper parameter, and lets the per-feature plans add a one-liner
instead of 30 lines of bash.

## Existing related upstream work

- `tests/nixos/fsync.nix` — the existing per-FS pattern; this plan
  generalises and replaces its inline boilerplate.
- No PRs.
- The Lix fork has nothing analogous either.

## Files touched

- `tests/nixos/lib/fs-scratch-volume.nix` — new NixOS test library
  module exporting `mkScratchVolume { fs, sizeMiB ? 1024, mountOpts ? [] }`
  and `withScratchVolume { fs, ... }: testScript -> mergedTestScript`.
- `tests/nixos/fsync.nix` — refactor to use the library (`withScratchVolumes [ "ext4" "btrfs" "xfs" ]`).
- `tests/nixos/lib/darwin-fs-probe.sh` — new bash helper for macOS
  runners: `darwin-fs-probe path/to/dir` prints the FS type and
  case-sensitivity capability. Used by Darwin-side tests that need to
  skip when the runner doesn't have an APFS case-sensitive test
  volume.
- `tests/functional/lib/scratch-volume.sh` — bash counterpart for
  functional tests: when `NIX_TEST_SCRATCH_FS=btrfs` is set, the
  helper uses a pre-mounted scratch dir from
  `$NIX_TEST_SCRATCH_DIR_BTRFS`; otherwise tests run on `$TEST_ROOT`
  as today.
- `flake.nix` — add `lib/fs-scratch-volume.nix` to the
  `nixosTestLibs` set (or wherever shared test code lives).
- `doc/manual/source/development/testing.md` — document the new
  fixture.

## Setting design

The fixture takes an FS-name → mount-recipe mapping table. Adding a
new FS (e.g. `f2fs`, `bcachefs`) is one row.

```nix
# tests/nixos/lib/fs-scratch-volume.nix
{
  ext4   = { mkfs = "mkfs.ext4 -F";  mount = "ext4";  pkgs = [ pkgs.e2fsprogs ]; };
  btrfs  = { mkfs = "mkfs.btrfs -f"; mount = "btrfs"; pkgs = [ pkgs.btrfs-progs ]; };
  xfs    = { mkfs = "mkfs.xfs -f";   mount = "xfs";   pkgs = [ pkgs.xfsprogs ]; };
  zfs    = { mkfs = "zpool create -f scratch /dev/vdb"; mount = "zfs"; pkgs = [ pkgs.zfs ]; needsKernelModule = true; };
  tmpfs  = { mkfs = null; mount = "tmpfs"; opts = [ "size=1G" ]; pkgs = []; };
}
```

ZFS support is gated on `boot.supportedFilesystems = [ "zfs" ]` in the
VM, which requires CONFIG_ZFS — Hydra's standard kernel ships with
out-of-tree ZFS, so this works.

## Commits (in order)

1. **`tests/nixos: add fs-scratch-volume library`**
   - New `tests/nixos/lib/fs-scratch-volume.nix` exporting
     `mkScratchVolume` and `withScratchVolumes`.
   - Five FS recipes: ext4, btrfs, xfs, zfs, tmpfs.
   - One self-test NixOS test that uses each recipe to mount a 256 MiB
     scratch volume, write a 64 MiB file, fsync, unmount, remount,
     verify content. Asserts the rig itself works on every supported
     FS before any feature test depends on it.

2. **`tests/nixos: refactor fsync.nix to use the new library`**
   - Replace the inline `for fs in ("ext4", "btrfs", "xfs"):` loop
     with `withScratchVolumes [ "ext4" "btrfs" "xfs" ]`.
   - Behaviour-preserving; no test changes, just deduplication.

3. **`tests: add darwin-fs-probe helper`**
   - `tests/nixos/lib/darwin-fs-probe.sh` (works as plain bash on
     macOS too): wraps `getattrlist` to print
     `apfs-case-sensitive` / `apfs-case-insensitive` /
     `hfs+` / `unknown`. Used by macOS tests that need to gate on the
     runner's volume type.
   - Unit smoke: shellcheck-clean, runs in 100ms.

4. **`tests/functional: add scratch-volume.sh helper for bash tests`**
   - Sources from `tests/functional/lib/`. When
     `NIX_TEST_SCRATCH_FS` is set, exposes
     `$SCRATCH_DIR` pointing at a pre-mounted FS-specific dir.
     Otherwise, exposes `$SCRATCH_DIR=$TEST_ROOT` (current behaviour).
   - This lets a single bash test conditionally exercise its FS-aware
     code path under different FSes by re-running with different
     env-var settings; CI launches it once per FS variant.

5. **`flake: register fs-scratch-volume in nixosTestLibs`**
   - Surface from `flake.nix` so other consumers (e.g. NixOS module
     tests) can import it.

6. **`doc: document the fs-scratch-volume fixture`**
   - In `doc/manual/source/development/testing.md`, a section
     "Filesystem-scoped tests" with the fixture API and an example.

## Tests

The fixture itself is tested by commit 1's self-test (every recipe
mounts, writes, syncs, remounts, reads back).

Per-feature use is tested by the FS-optimisation plans that adopt it.

## Risk and rollback

Risk: very low.

- `withScratchVolumes [ "zfs" ]` requires CONFIG_ZFS in the VM kernel.
  If a test environment lacks it, the test errors at VM build time
  with a clear "ZFS module not available" message rather than at
  runtime. Mitigation: document; fall back to skipping at the
  `pkgs.zfs.meta.broken` gate.
- ZFS dataset cleanup can be flaky if the test exits abnormally
  (zpool stays imported). Mitigation: `Finally` cleanup +
  `zpool export -f` in the test teardown.

Rollback: trivial — each test that adopts the library can revert to
its inline version.

## Dependencies

- None on other plans here or in the FS folders.
- This plan **must** ship before any new per-FS test in the FS-opt
  plans, otherwise we re-invent the rig N times.

## Definition of done

- [ ] Library landed at `tests/nixos/lib/fs-scratch-volume.nix`.
- [ ] `tests/nixos/fsync.nix` refactored, no behaviour change.
- [ ] All five recipes self-tested by the library's self-test.
- [ ] Darwin probe helper landed.
- [ ] Functional-test helper landed.
- [ ] Manual section added.
