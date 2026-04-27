# 05 — Opt-in btrfs subvolume per build directory

## Goal

Add a `build-dir-use-btrfs-subvolume` setting that, when enabled and the
configured `build-dir` lives on btrfs, creates each per-build temp
directory as a btrfs subvolume (`btrfs subvolume create`) instead of an
ordinary `mkdir`. On cleanup the subvolume is destroyed via
`btrfs subvolume delete`.

Optional follow-up: a `--reflink-output` mode that (with plan 04's
`cloneFile`) clones build outputs from the per-build subvolume into
`/nix/store` rather than copying.

## Why

The `derivation-builder.cc` lifecycle creates a build directory under
`build-dir` (default `${nixStateDir}/builds`), runs the builder, and on
completion either moves outputs out and `rm -rf`'s the rest, or
preserves the directory for `keep-failed`. For builds with many small
files (Linux kernel: ~75 k files, Chromium: ~600 k, Rust toolchain
intermediates: ~1 M), the cleanup `rm -rf` is O(files) and dominates
post-build wall-time.

`btrfs subvolume create` and `btrfs subvolume delete` are O(1) at the
subvolume level — the kernel just discards the entire subvolume's tree
in a background async deletion. Anecdotally, Linux kernel builds
benchmark this as 10-30× faster cleanup on a moderately-aged btrfs
volume.

Secondary benefit, composable with plan 04: when `$out` is moved into
`/nix/store`, `cloneFile`-ing each output from the subvolume to the
store inherits the build's data extents instead of copying. Combined
with btrfs cross-subvolume reflinks, the move becomes near-instant.

ZFS analogue (`zfs create dataset`) is heavier-weight (requires a parent
dataset configured for it) and has worse small-file ergonomics; out of
scope. ZFS-specific subvolume support could be a follow-up plan if
demand exists.

## Existing related upstream work

- No PRs or issues address per-build subvolume creation.
- [Issue #1272](https://github.com/NixOS/nix/issues/1272) "Allow some
  derivations to hardlink to other files in the store" — the original
  concern (build inputs as hardlinks) is solved differently on Linux
  (bind-mounts in chroot), but the `keep-failed` cleanup pain is the
  same family of problem.
- Plan 04 here — composes with this plan for fast output-move.
- APFS plan 05 (Darwin `copyfile(COPYFILE_CLONE)`) — same goal on Darwin
  via clones; Darwin doesn't have a "subvolume" concept on APFS so the
  approach must remain copy-based there. This plan is Linux-specific.

## Files touched

- `src/libstore/include/nix/store/local-settings.hh` — new setting
  `build-dir-use-btrfs-subvolume` (default `auto`).
- `src/libstore/unix/build/derivation-builder.cc` — at the build
  directory creation site (search for `tmpDir = createTempDir(...)`),
  branch on the setting + FS type. At cleanup, branch on whether the
  dir was created as a subvolume.
- `src/libutil/include/nix/util/file-system.hh` +
  `src/libutil/unix/file-system.cc` — `bool createBtrfsSubvolume(path)`,
  `bool deleteBtrfsSubvolume(path)` returning `false` when not on btrfs
  or when the kernel rejects the ioctl.
- `tests/nixos/build-subvolume.nix` — new NixOS test.
- `doc/manual/source/command-ref/conf-file.md` — document setting.
- `doc/manual/rl-next/build-dir-btrfs-subvolume.md` — release note.

## Setting design

```
build-dir-use-btrfs-subvolume = auto    # default: probe build-dir, use subvolumes if FS is btrfs
                              | always   # error if build-dir is not btrfs (helps catch misconfiguration)
                              | never    # disable; current behaviour
```

`auto` is safe to default on at GA: probe is `statfs(buildDir)` checking
`f_type == BTRFS_SUPER_MAGIC`, cached at daemon startup.

## Commits (in order)

1. **`libutil: add btrfs subvolume helpers`**
   - `bool createBtrfsSubvolume(path)` — `ioctl(parent_fd, BTRFS_IOC_SUBVOL_CREATE, &args)` with the basename. Returns `false` on `EOPNOTSUPP` / `EPERM` (non-btrfs / no privilege).
   - `bool deleteBtrfsSubvolume(path)` — `ioctl(parent_fd, BTRFS_IOC_SNAP_DESTROY, &args)`. Returns `false` if the path isn't a subvolume.
   - `bool isBtrfsSubvolume(path)` — `statx(STATX_ATTR_BTRFS_SUBVOL)` (Linux 5.8+) or fallback via `ioctl(BTRFS_IOC_INO_LOOKUP)`.
   - Unit test: skipped when `/tmp` isn't btrfs; on btrfs CI runners (when present) verify create/delete round-trip.

2. **`libstore: add build-dir-use-btrfs-subvolume setting (default auto, probe at startup)`**
   - Parse the enum; cache the `auto` resolution after probing `buildDir`'s `f_type`.
   - No call sites use it yet; behaviour unchanged.

3. **`libstore: create per-build directory as btrfs subvolume when enabled`**
   - In `derivation-builder.cc`, replace `createTempDir(buildDir, ...)` with a small helper that, when the setting is active, calls `createBtrfsSubvolume(buildDir / fmt("nix-build-%d-%d-XXXXXX", ...))` after the temp-name dance.
   - Track per-build whether the dir was created as a subvolume (a `bool` on the builder state). Cleanup path branches accordingly: subvolume → `deleteBtrfsSubvolume`; ordinary → `deletePath` as today.
   - `keep-failed` semantics: the subvolume persists; `nix-store --gc` cleanup of `${buildDir}` already handles directories, so it's compatible.

4. **`libstore: reflink build outputs into the store when on btrfs (uses cloneFile from plan 04)`**
   - In `moveOutputToTempDir` (`derivation-builder.cc:1756-1775`), when the build directory and the store live on the same btrfs filesystem, prefer `cloneFile(buildPath, storePath)` over `copyRecursive`. Drop back to `copyRecursive` (which itself uses `copy_file_range` from PR #15410) on `false`.
   - Drops the TODO comment. Cite plan 04 + PR #15410 + the Sergei Zimmerman commit.

5. **`tests: NixOS test for btrfs subvolume build dirs`**
   - VM with btrfs scratch disk, mount as `build-dir`, build a derivation with ~10 k tiny files in `$out`. Time the build with `build-dir-use-btrfs-subvolume = always` vs `= never`. Assert the `always` run finishes in less than 70% of the `never` time. Also assert that mid-build the build-dir is reported as a subvolume by `btrfs subvolume show`.

6. **`doc: document the setting and the cleanup speedup`**
   - Manual entry under "Advanced settings" / "Local store".
   - Release note: experimental, opt-in via `auto`, what it gives you, what it doesn't (no checksumming on the build-dir subvolume; that's fine because builds are ephemeral).

## Availability

- `BTRFS_IOC_SUBVOL_CREATE` and `BTRFS_IOC_SNAP_DESTROY` are stable since 2007. No version gating needed.
- The build user must have permission to create subvolumes in `build-dir`. On modern btrfs (>= 4.18) any user can create subvolumes in a directory they can write to. On older kernels, root-only — `auto` mode falls back to `mkdir` silently in that case (the ioctl returns `EPERM`).

## Risk and rollback

Risk: medium.

- **Inode-table cost**: each subvolume eats one inode in the parent and one in the subvolume tree. For a daemon doing 10⁵ builds/day, that's still trivial.
- **`keep-failed`**: persistent subvolumes take a tiny bit longer to delete via `nix-store --gc`. Acceptable.
- **`btrfs subvolume sync`**: deletes are async; in pathological cases the disk-space accounting lags. Mitigation: document it; in `auto` mode, skip subvolume creation when free disk space is below 10% (would surprise users who rely on `nix-collect-garbage`).
- **Sandboxing interaction**: the subvolume is created by the daemon, then bind-mounted into the chroot. No interaction issues observed in similar tooling (`buildkit`, `nspawn`).

Rollback: revert commits 3-6; the setting stays as a no-op. Existing
build dirs created as subvolumes are still deletable by `rm -rf` (just
slower) — no migration needed.

## Dependencies

- **Hard:** plan 04's `cloneFile` for commit 4. Without commit 4 the
  cleanup speedup still works; commit 4 is the additive output-move
  speedup.
- **Soft:** plan 03's NixOS btrfs scratch-volume rig. If plan 03 is
  in, commit 5 is trivial.
- Independent of plans 01, 02, 06, 07, 08.

## Out of scope

- ZFS dataset analogue (deferred until demand).
- `btrfs send/receive` for `nix copy` between btrfs stores.
- Snapshot-based rollback of failed builds (could compose; not now).

## Definition of done

- [ ] All commits land in order.
- [ ] Benchmarks in PR description: kernel build cleanup time `auto` vs `never` on a 1-month-old btrfs scratch volume.
- [ ] NixOS test green.
- [ ] Manual + release note updated.
- [ ] Verified by hand on btrfs that mid-build `btrfs subvolume list /nix/var/nix/builds` shows the per-build subvolume.
