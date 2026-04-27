# 02 — Make `gc-reserved-space` actually reserve space on compressed filesystems

## Goal

Stop the `posix_fallocate(reservedSize)` + zero-fill at
`src/libstore/local-store.cc:209-222` from being silently compressed to
~zero bytes on btrfs/ZFS volumes with transparent compression. Either
fill with incompressible data, set `FS_NOCOW_FL` + `chattr +c`-no on the
reserve file before allocation, or use a known-incompressible byte
pattern.

## Why

The "GC reserve" is a single file Nix preallocates so that
`nix-store --gc` always has *some* room to write the SQLite WAL even
when the disk is "full". When the user later runs out of space, Nix
truncates the reserve file, freeing those bytes for the GC's own
metadata writes.

On btrfs with `compress=zstd` (the default in many distributions) and on
ZFS with `compression=on/lz4`, a file full of `'X'` characters
(`local-store.cc:214`) compresses to a few hundred bytes regardless of
the `reservedSize` setting (default 8 MiB). When the disk fills up,
truncating the reserve file frees a few hundred bytes — not enough for
the GC to make progress. The daemon then crashes inside SQLite with the
stack trace in [issue #3808](https://github.com/NixOS/nix/issues/3808).

`posix_fallocate` itself is not the problem — both btrfs and ZFS honour
allocation requests at the extent level. But the fallback path
(`writeFull(fd, std::string(size, 'X'))` at line 214) writes
compressible data. And on filesystems that do delayed allocation +
compression, `posix_fallocate` may also become a no-op when the writer
goes through the page cache.

Two mitigations, applied together:

1. **Fill with incompressible data** — `/dev/urandom` bytes, or a
   counter-encoded pattern that defeats both LZ4 and zstd. This works
   regardless of FS / mount options.
2. **Disable CoW + compression on the reserve file specifically** — `chattr +C` (NoCOW implies nodatasum implies no compression on btrfs) before any writes. On ZFS, set the dataset property — out of scope for the daemon, but documented.

## Existing related upstream work

- [Issue #3808](https://github.com/NixOS/nix/issues/3808) — exact symptom; reporter's hypothesis (compression of zero-filled reserve) is correct.
- [Issue #564](https://github.com/NixOS/nix/issues/564) — "Make garbage collector work if there is no free space". Broader umbrella; the reserve file is one of the workarounds.
- [Issue #2376](https://github.com/NixOS/nix/issues/2376) — "Reserve a couple 'allowance' files for inode-exhaustion situations". Related axis (inodes vs bytes); cite for context.
- No PR addresses any of these.

## Files touched

- `src/libstore/local-store.cc` — the reserve-file create-and-fill block at `:198-227`.
- `src/libutil/include/nix/util/file-system.hh` + `src/libutil/unix/file-system.cc` — small helper `void writeIncompressibleBytes(Descriptor, off_t)` (counter-pattern; deterministic so easy to test).
- Reuses `disableCoW` helper from plan 01.
- `tests/nixos/fsync.nix` or new `tests/nixos/gc-reserve.nix` — test that on btrfs+zstd, after creating the reserve, `du --apparent-size` ≈ `du` (i.e. no compression savings).
- `doc/manual/rl-next/gc-reserve-incompressible.md` — release note.
- `doc/manual/source/command-ref/conf-file.md` — extend the `gc-reserved-space` doc with a note about compressed filesystems.

## Commits (in order)

1. **`libutil: add writeIncompressibleBytes helper`**
   - Writes a 64-bit counter (little-endian) repeated for `len` bytes. Deterministic, defeats LZ4/zstd because every 8-byte block has different leading bits.
   - Unit test: write 1 MiB to a temp file, verify size with `fstat`, verify a sample byte at offset N matches the counter.

2. **`libstore: fill GC reserve with incompressible data + disable CoW on btrfs`**
   - Replace `writeFull(fd.get(), std::string(gcSettings.reservedSize, 'X'))` with `writeIncompressibleBytes(fd.get(), gcSettings.reservedSize)`.
   - Before the fallocate/write, attempt `disableCoW(fd.get())` (from plan 01). On btrfs, this propagates "no compression" too. On other FS this is a no-op.
   - Order: open → `disableCoW` → `posix_fallocate` → `writeIncompressibleBytes` (only if `posix_fallocate` failed). The size check at `:200` becomes "size matches AND extents are at least allocated"; a follow-up `fstat` after creation verifies `st_blocks * 512 ≈ size`.

3. **`libstore: re-create reserve at startup if it appears compressed`**
   - At `LocalStore` startup, after the existing `st->st_size != gcSettings.reservedSize` check, also detect `st_blocks * 512 < st_size * 0.9` (a 10% slack handles partial fragmentation/sparse). If detected, log `warn("garbage-collector reserve file appears compressed; recreating")` and rebuild it via the commit-2 path.
   - This auto-fixes existing stores migrating across this change without a manual step.

4. **`tests: NixOS test for GC reserve incompressibility`**
   - `tests/nixos/gc-reserve.nix`: btrfs scratch volume, mount with `compress=zstd:3`, run a Nix command that triggers `LocalStore` init, assert `stat -c '%b' /mnt/var/nix/db/reserved` × 512 ≥ `gc-reserved-space`. Optionally repeat for ZFS via `zfs set compression=lz4`.

5. **`doc: release note + manual update for GC reserve on compressed FS`**
   - Release note: closes #3808; existing stores auto-migrate at next daemon start; users wanting to force the migration can `rm /nix/var/nix/db/reserved` and restart.
   - Manual: in the `gc-reserved-space` entry, add a paragraph: "On filesystems with transparent compression (btrfs `compress=*`, ZFS `compression=on`), Nix writes incompressible bytes and sets `FS_NOCOW_FL` so that the reserve actually consumes disk space."

## Tests

- Unit (commit 1).
- NixOS (commit 4).
- Manual: on btrfs+zstd, `mkfs.btrfs ; mount -o compress=zstd:3 ; nix-daemon ; du -sb /nix/var/nix/db/reserved` should report close to 8 MiB.

## Risk and rollback

Risk: low.

- The bytes written are still meaningless from Nix's perspective; only the disk-space accounting changes.
- A pre-existing reserve file that *is* compressed will be detected by commit 3 and recreated — one-time disk hit of ~8 MiB. Negligible.
- `disableCoW` on the reserve disables btrfs checksumming for that file; that's fine (it's an opaque scratch file).

Rollback: revert any subset of commits 2-4. Commit 1's helper stays useful (could be reused elsewhere). The store keeps working with whatever reserve representation was last written.

## Dependencies

- **Soft:** plan 01's `disableCoW` helper. If plan 01 lands first, plan 02 reuses it; if plan 02 lands first, plan 01 lifts the helper out into the shared header.
- Independent of all other plans here and in `../store-apfs-optimisations/`.

## Definition of done

- [ ] All commits land in order.
- [ ] Unit + NixOS tests green.
- [ ] Issue #3808 referenced (closed).
- [ ] Manual updated with the compression-aware paragraph.
- [ ] Verified by hand on a btrfs+zstd volume that an 8 MiB reserve actually occupies 8 MiB.
