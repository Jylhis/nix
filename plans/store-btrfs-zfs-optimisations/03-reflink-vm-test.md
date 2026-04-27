# 03 — NixOS VM test for cross-VFS reflink on btrfs

## Goal

Add a NixOS VM test that exercises [issue #5513](https://github.com/NixOS/nix/issues/5513):
copy a large file from a build's `$TMPDIR` (on the same btrfs filesystem
as `/nix/store` but mounted via a separate bind-mount) into the store
using `cp --reflink=always`, verify it succeeds, and verify that the
destination shares extents with the source.

## Why

The original [issue](https://github.com/NixOS/nix/issues/5513) reported
`Invalid cross-device link` from `cp --reflink=always` when copying a
file from `$TMPDIR` (under `/nix/var/nix/tmp/`) to `$out` (under
`/nix/store`), even though both paths live on the same btrfs filesystem.
Reason: VFS used to forbid `FICLONE` across mount boundaries, even when
the underlying filesystem was the same. Linux 5.18 removed that
restriction; modern Nixpkgs ships kernels well past 5.18.

Eric Lehmann (`Ericson2314`) explicitly reopened the issue asking for a
NixOS VM test that proves this stays working — *not* a code change in
Nix, just a regression guard. This plan is exactly that.

The same test machinery has follow-on value for plans 04 (FICLONE
helper) and 05 (btrfs subvolume build dir): both want a "btrfs scratch
volume in CI" rig.

## Existing related upstream work

- [Issue #5513](https://github.com/NixOS/nix/issues/5513) — the directly tracked item; reopened with the explicit request for this test.
- [PR #15410](https://github.com/NixOS/nix/pull/15410) — adds `copy_file_range` in `FdSource::drainInto`; once merged, the same NixOS test gives partial coverage of plan 04's `FICLONE` helper too.
- [`tests/nixos/fsync.nix`](../../tests/nixos/fsync.nix) — the canonical pattern for "spin up a btrfs/xfs/ext4 scratch disk in a NixOS VM and exercise Nix against it". This plan copies its structure.

## Files touched

- `tests/nixos/reflink-cross-vfs.nix` — new file.
- `flake.nix` — register the test in `hydraJobs.tests` (locate the existing pattern alongside `fsync`).
- `doc/manual/rl-next/reflink-cross-vfs-test.md` — small release note: "Added a NixOS test that guards cross-VFS reflinks remain working on btrfs."

## Commits (in order)

1. **`tests/nixos: add reflink-cross-vfs test`**
   - New file modelled on `tests/nixos/fsync.nix`.
   - VM with `boot.supportedFilesystems = [ "btrfs" ]`, an empty 1 GiB scratch disk.
   - Test script:
     1. `mkfs.btrfs -f /dev/vdb && mkdir -p /mnt/store /mnt/tmp && mount /dev/vdb /mnt`.
     2. `mkdir -p /mnt/store /mnt/tmp` (two subdirs *inside* the same FS but different bind-mount points).
     3. `mount --bind /mnt/store /mnt/store && mount --bind /mnt/tmp /mnt/tmp` (creates the cross-VFS condition the original issue hit).
     4. Generate a 64 MiB random file in `/mnt/tmp/src`.
     5. `cp --reflink=always /mnt/tmp/src /mnt/store/dst` — must succeed.
     6. Compare `stat -c '%b' /mnt/tmp/src` and `stat -c '%b' /mnt/store/dst`; they should differ trivially (only metadata extents differ). The strong assertion is `filefrag -v` reports the same physical extents on both.
     7. Modify a single byte in `/mnt/tmp/src`; re-check that `dst` still has the *original* extent (CoW on write splits the share).
   - Cite the LWN/lore link (the [josef@toxicpanda.com series](https://lore.kernel.org/linux-btrfs/cover.1645194730.git.josef@toxicpanda.com/T/#mf251325026fe2e15ed5119856bf654ba4f0d298b)) in a comment.

2. **`flake: register reflink-cross-vfs in hydraJobs.tests`**
   - Mirror the existing `fsync` registration.

3. **`doc: release note for the reflink VM test`**
   - One-line note: "Added a NixOS regression test for cross-VFS reflinks on btrfs (closes #5513)."

## Tests

This *is* the test. No additional unit coverage needed.

Local invocation: `nix build .#hydraJobs.tests.reflink-cross-vfs`.

## Risk and rollback

Risk: none — no production code change.

- A possible flake mode if a future kernel regression breaks cross-VFS reflinks: that's exactly what we want to catch. Treat as a feature, not a flake.
- If the test ever depends on a specific kernel feature flag, gate via `pkgs.linuxKernel.kernels.linux_X_Y` rather than skipping.

Rollback: trivial revert.

## Dependencies

- None.
- Composes nicely with plan 04 (when plan 04 lands, extend this test to also exercise the in-process `cloneFile()` helper, not just `cp --reflink=always`).
- Composes with plan 05 (subvolume build dir) — both want the same btrfs CI rig.

## Definition of done

- [ ] Test file lands and is green on Hydra's `aarch64-linux` and `x86_64-linux` runners.
- [ ] Issue #5513 is closed by the merging commit.
- [ ] Release note merged.
- [ ] Comment in the test cites the upstream Linux 5.18 patch series.
