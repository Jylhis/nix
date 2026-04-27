# 05 — Reflinking `copyFile` and `copyRecursive`

## Goal

Replace the byte-by-byte `std::filesystem::copy_file` fallback in `copyFile()` (`src/libutil/file-system.cc:581-620`) and the analogous read/write loop inside `copyRecursive()` with platform-native reflink calls when source and target sit on the same volume:

- Darwin: `copyfile(src, dst, NULL, COPYFILE_CLONE | COPYFILE_DATA | COPYFILE_NOFOLLOW)` — kernel-side; falls back to a regular copy when the volume doesn't support cloning.
- Linux: `ioctl(dst_fd, FICLONE, src_fd)` then fall back to `copy_file_range(2)`, then a regular read/write.
- Else: current behaviour.

## Why

The two hottest callers in production are:

1. The TODO at `src/libstore/unix/build/derivation-builder.cc:1766` — `moveOutputToTempDir` round-trips every output byte through serialise/deserialise just to break stale FDs and create a fresh inode set. A single `clonefile`-or-equivalent per file would replace that whole loop. Commit `52011de1b` (Apr 21 2026, by Sergei Zimmerman) already did the first half of the rewrite (NAR round-trip → `copyRecursive`); it explicitly says "could start using reflinking in the future too". This plan is that future.
2. `src/libstore/unix/build/freebsd-derivation-builder.cc:296,357,365` — chroot setup copies every input dependency into a per-build sandbox. On a Nix store with thousands of paths, this dominates build start-up.

For the cross-volume case (e.g. a tmpfs `/tmp` → `/nix`), the fallback is unchanged.

## Existing related upstream work

- Commit `52011de1b` "libstore: Use copyRecursive when copying FOD outputs" (Apr 21 2026) — the explicit hand-off site.
- [Issue #5513](https://github.com/NixOS/nix/issues/5513) "Test: Readonly nix store mount still allows for reflinks" — open since 2022; tangentially relevant (reflinking through a read-only bind mount); cite for context.
- No PRs touch `copyfile` / `FICLONE` in either NixOS/nix or Lix.

## Files touched

- `src/libutil/include/nix/util/file-system.hh` — public API stays, internals get a helper.
- `src/libutil/file-system.cc` — primary change.
- `src/libutil/unix/file-system.cc` — Darwin `copyfile`, Linux `FICLONE`, FreeBSD/OpenBSD fallback.
- `src/libstore/unix/build/derivation-builder.cc` — drop the TODO once the implementation is wired through `copyRecursive`.
- `src/libstore/unix/build/freebsd-derivation-builder.cc` — no source change needed; benefits transparently.
- `src/libutil-tests/file-system.cc` — unit tests.
- `doc/manual/rl-next/copyfile-clone.md` — release note.

## Commits (in order)

1. **`libutil: factor copyFileContents helper`**
   - Pull the body of `copyFile`'s "is_regular_file" branch into a dedicated `copyFileContents(from_fd, to_fd, length)` helper that returns `bool` (true if a fast path was used).
   - No behaviour change.

2. **`libutil: use copyfile(COPYFILE_CLONE) on Darwin in copyFileContents`**
   - On `__APPLE__`: try `copyfile(src, dst, NULL, COPYFILE_CLONE | COPYFILE_DATA | COPYFILE_NOFOLLOW | COPYFILE_EXCL)`. On `EXDEV` / `ENOTSUP` fall through to the existing read/write loop.
   - Preserve mtime / mode handling — `COPYFILE_DATA` only copies data, we set metadata explicitly afterwards as the current code does.

3. **`libutil: use FICLONE / copy_file_range on Linux in copyFileContents`**
   - On `__linux__`: `ioctl(dst, FICLONE, src)`; on failure (EOPNOTSUPP, EXDEV, EINVAL) fall through to `copy_file_range`; on its failure fall through to read/write.
   - Reuses the existing read/write loop unchanged.

4. **`libstore: drop reflink TODO from derivation-builder`**
   - Replace the `// TODO: Use copyRecursive here and make use of reflinking.` comment with a `// Reflinks via copyFileContents when the FS supports it; falls back to data copy.` since `copyRecursive` now uses `copyFileContents` under the hood.
   - Cite the Sergei Zimmerman commit + this plan in the commit body.

5. **`tests: cover reflink fast path`**
   - Unit: build a temp tree, copy it via `copyRecursive`, assert content equality on every supported OS. On `__APPLE__` and `__linux__` (when running on a btrfs/xfs/zfs scratch dir), additionally assert `st.st_blocks` is unchanged on the destination compared to the source — i.e. data extents are shared. CI builders are unlikely to provide btrfs, so gate the strong assertion on a `NIX_TEST_REFLINK=1` env var.
   - Functional: extend `tests/functional/build-hook-ca-floating.nix` (or similar) with a derivation that produces a 256 MiB output, time the post-build move-to-store, fail if it regresses past a threshold.

6. **`doc: release note for copy reflinking`**
   - What changed; who notices (mac users on APFS, Linux users on btrfs/xfs/zfs); pointer to `copyfile(3)` and `ioctl(2) FICLONE` man pages; explicit mention that cross-volume copies are unchanged.

## Availability

- `copyfile(3)` with `COPYFILE_CLONE` is macOS 10.12+ — universally available on supported macOS versions.
- `FICLONE` ioctl is Linux 4.5+ (2016) — well below Nixpkgs's minimum supported kernel.
- No SDK gating needed.

## Risk and rollback

Risk: medium.

- Reflinks change `st_blocks` accounting; tools that estimate disk usage from `du` see different numbers. Document this.
- Cross-volume copy must remain a real copy. Verify the `EXDEV` fallback by mounting tmpfs in CI and copying across.
- COPYFILE_CLONE preserves the source's NFS / xattr state in some macOS versions; canonicalisation already strips xattrs (`posix-fs-canonicalise.cc`), but verify.

Rollback: revert any subset of commits 2-4; the helper from commit 1 is retained because the read/write fallback uses it.

## Dependencies

- **Soft**: plan 01 establishes the macOS runtime-feature pattern (`__builtin_available` vs runtime-`EINVAL`); reuse it here.
- Independent of plans 02, 03, 04.
- Independent of plan 06 (different syscalls — `copyfile(COPYFILE_CLONE)` for files in transit; `clonefile(2)` for store-internal dedup).

## Out of scope

- Cross-store reflinks (would require a daemon protocol change).
- Reflinking during chroot bind-mount setup (different code path; covered if anyone wants a follow-up).

## Definition of done

- [ ] Unit + functional tests green on Linux and macOS CI.
- [ ] Benchmark numbers in PR description (256 MiB derivation post-build move; chroot setup with N inputs on FreeBSD if reachable).
- [ ] Release note merged.
- [ ] Cross-volume fallback verified by hand on a tmpfs-mounted `/tmp`.
