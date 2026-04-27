# 04 — Explicit `FICLONE` `cloneFile` helper for Linux

## Goal

Introduce `bool cloneFile(const path & src, const path & dst)` whose
Linux backend issues `ioctl(dst_fd, FICLONE, src_fd)`, returning `false`
on `EOPNOTSUPP` / `EXDEV` / `EINVAL` / `ENOSYS` so callers can fall back
to a regular copy. This is the **explicit** reflink primitive, distinct
from PR #15410's `copy_file_range` path which is *implicit* — kernel
chooses whether to share extents.

Same helper signature as APFS plan 06; the two plans share commit 1.

## Why

PR #15410 puts `copy_file_range` inside `FdSource::drainInto`, so
existing callers transparently get CoW where the kernel decides it's
worth it. That's the right choice for the streaming NAR
serialise/deserialise path.

But there are at least three call sites where we want to *control*
whether sharing happened, not just hope:

1. **`optimise-store.cc`** (auto-optimise dedup) — clones break the
   `nlink == 1 ⇒ orphan` GC short-circuit; we need to know we actually
   shared extents before we can switch the GC contract per path. APFS
   plan 06 covers this on Darwin via `clonefile(2)`. On Linux, `FICLONE`
   is the equivalent.
2. **`derivation-builder.cc:1756-1775`** — the `moveOutputToTempDir`
   round-trip explicitly notes "could start using reflinking in the
   future too" (see APFS plan 05). When we want to *guarantee* a fast
   move (and fail loudly if the FS doesn't cooperate), `FICLONE` lets us
   distinguish "shared extents" from "fell back to data copy" so the
   benchmark can prove the speedup.
3. **Future cross-store `nix copy` from one local store to another on
   the same btrfs volume** — instead of `cp -r`, do a per-file
   `FICLONE`. Out of scope for this plan, but the helper enables it.

`copy_file_range` cannot signal "did I CoW?" to the caller — it returns
the byte count regardless of whether the kernel chose data-copy or
extent-sharing. `FICLONE` is binary: either the inode now shares extents
with the source, or it failed.

## Existing related upstream work

- **[PR #15410](https://github.com/NixOS/nix/pull/15410)** "libutil/serialise: Use zero-copy copy_file_range for copying FdSource → FdSink" — **complementary, not overlapping**. `copy_file_range` is the implicit path; `FICLONE` is the explicit path. Comment on the PR linking this plan; ask Sergei whether he wants both layered together or stacked.
- **APFS plan 06** (`../store-apfs-optimisations/06-clonefile-dedup.md`) — defines the cross-platform `cloneFile` interface. Whichever plan ships first introduces the helper; the other reuses it.
- **APFS plan 05** (`../store-apfs-optimisations/05-copyfile-clone.md`) — the Darwin `copyfile(COPYFILE_CLONE)` analogue inside `copyFileContents`. Linux side of *that* plan is already covered by PR #15410's `copy_file_range`. So plan 04 here is *not* the Linux equivalent of APFS plan 05; it's the Linux equivalent of APFS plan 06.
- No other upstream work.

## Files touched

- `src/libutil/include/nix/util/file-system.hh` — add `bool cloneFile(const std::filesystem::path & src, const std::filesystem::path & dst)`.
- `src/libutil/unix/file-system.cc` — Linux `FICLONE` body. (Darwin body comes from APFS plan 06.)
- `src/libutil/meson.build` — `cxx.has_header('linux/fs.h')` guard.
- `src/libutil-tests/file-system.cc` — unit test as in APFS plan 06.
- `doc/manual/rl-next/clone-file-helper.md` — release note (consolidate with APFS plan 06's release note if both ship together).

## Commits (in order)

1. **`libutil: add cloneFile helper (Linux FICLONE backend)`**
   - `bool cloneFile(const path & src, const path & dst)`.
   - Linux: open `src` `O_RDONLY|O_CLOEXEC|O_NOFOLLOW`, create `dst` `O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|0600`, `ioctl(dst_fd, FICLONE, src_fd)`. Translate `EOPNOTSUPP`/`EXDEV`/`EINVAL`/`ENOSYS` → return `false` (delete the empty `dst`). All other errors throw.
   - Other platforms (until APFS plan 06 lands): return `false`.
   - Cache `ENOSYS` per-process via `std::atomic_flag` (same pattern as PR #15410's `copyFileRangeUnsupported`).
   - Unit test: clone a 1 MiB file, verify content equality and `st_blocks` accounting (assert destination's `st_blocks` matches source's).

2. **`libutil: add cloneRange convenience for partial reflinks`** *(optional, can defer)*
   - `bool cloneRange(src, dst, src_offset, dst_offset, length)` wrapping `BTRFS_IOC_CLONE_RANGE` / `FICLONERANGE`. Caller controls offsets — useful for sparse NAR restoration. No call sites in this plan; exists for future use.

3. **`libstore: opportunistic FICLONE in copyRecursive's per-file copy`**
   - In `copyFile` (`src/libutil/file-system.cc:581-620`), before invoking the read/write fallback, try `cloneFile(src, dst)`; if it returns `true`, set mtime+mode and return. If `false`, fall through to the existing path (which on Linux now also has `copy_file_range` thanks to PR #15410).
   - This shaves the kernel-side `EOPNOTSUPP` retry from the hot path on btrfs/XFS/ZFS where `FICLONE` is known to work.

4. **`tests: extend reflink-cross-vfs NixOS test to exercise cloneFile`**
   - In the test from plan 03, additionally invoke a small helper binary that calls `nix::cloneFile(src, dst)` and asserts `true` on btrfs.

5. **`doc: release note + manual update`**
   - Brief note: "New `cloneFile` helper now used by `copyRecursive` on Linux (btrfs/XFS/ZFS) and Darwin (APFS); falls back to `copy_file_range` then read/write on unsupported filesystems."

## Availability

- `FICLONE` is in Linux 4.5+ (April 2016). Well below Nixpkgs's minimum supported kernel.
- `BTRFS_IOC_CLONE_RANGE` predates `FICLONERANGE`; ignore (use `FICLONERANGE` only).
- No SDK gating needed; just `#include <linux/fs.h>` guarded by `cxx.has_header`.

## Risk and rollback

Risk: medium.

- Adding `cloneFile` to `copyRecursive`'s per-file copy could regress when the destination already exists (we open `O_EXCL`). The current `copy_file_range` path doesn't have this issue because it writes into an open FD provided by the caller. Mitigation: only use `cloneFile` when the destination doesn't exist; for overwrite cases, use the existing path.
- `st_blocks`-based test assertions are flaky on filesystems with delayed allocation. Use `filefrag -v` to compare physical extents instead, or gate the strong assertion behind `NIX_TEST_REFLINK=1`.

Rollback: revert any subset of commits 2-4. Commit 1's helper is independently useful.

## Dependencies

- **Hard:** PR #15410 should land first — once it's in, the implicit `copy_file_range` path covers most callers, and this plan adds the explicit-control path on top. If #15410 stalls past two months, lift its `HAVE_COPY_FILE_RANGE` build infrastructure into commit 1 here.
- **Soft:** APFS plan 06 — share the helper signature. Whichever lands first writes the cross-platform `file-system.hh` declaration.
- **Soft:** plan 03 here — if plan 03's NixOS test infrastructure is already in place, plan 04's commit 4 is trivial.

## Out of scope

- Switching `optimise-store.cc` to clones (covered in APFS plan 06).
- Cross-store reflinks via daemon protocol (mentioned as future use; not in this plan).

## Definition of done

- [ ] All commits land in order.
- [ ] Unit + NixOS tests green.
- [ ] PR description benchmark: `cp --reflink=always /tmp/big /nix/store/big.test` time before and after, on a btrfs scratch volume.
- [ ] Release note merged.
- [ ] APFS plan 06's `cloneFile` declaration matches this signature.
