# 07 — Treat ZFS scrub-detected `EIO` as "path is broken; substitute"

## Goal

When `nix-store --repair-path` (or any read-while-verify path) hits
`EIO` reading a store path's contents, classify the path as "corrupted,
needs replacement" and proceed to re-fetch from a substituter, instead
of bailing out with `"reading from file: Input/output error"`.

## Why

ZFS detects bit-rot and unrecoverable hardware errors at read time:
when a checksum mismatches and the parity / mirror copy is also bad,
`zpool status -v` lists the affected files and `read(2)` on those files
returns `EIO`. This is the *correct* signal that the data is gone; the
filesystem is telling us truthfully.

[Issue #8121](https://github.com/NixOS/nix/issues/8121) reports that
`nix-store --repair-path` aborts on `EIO` instead of treating it as
"this path needs to be re-fetched". That defeats the purpose of
`--repair-path`: it's exactly the right command for this situation, and
exactly the situation where it fails.

The general "read failed → unverifiable → repair" path already exists
for hash-mismatch cases. We need to extend it to the system-error case
where the read itself failed.

This is btrfs/ZFS/MD-RAID-relevant in different ways:

- **ZFS**: bit-rot detection via checksums + parity scrub; `EIO` on read
  of corrupted blocks. The motivating case.
- **btrfs**: same model as ZFS — `EIO` on checksummed read of corrupted
  blocks. Less common in the wild but identical handling.
- **MD-RAID + ext4/xfs**: `EIO` propagated up from the MD layer when all
  copies are bad. Rare but possible.
- **dm-integrity**: also raises `EIO`.

So while motivated by ZFS, the fix is filesystem-agnostic.

## Existing related upstream work

- [Issue #8121](https://github.com/NixOS/nix/issues/8121) — directly
  closed by this plan.
- [Issue #2735](https://github.com/NixOS/nix/issues/2735) "nix-store
  --verify --check-contents fails on paths with i/o errors" — same
  symptom in the `--verify` path. Plan 07 should fix both: extend the
  EIO handling to `--verify --check-contents` too.
- [Issue #11148](https://github.com/NixOS/nix/issues/11148) "empty
  derivation and can't repair the various wrong store paths, thus
  blocking nixos-rebuild" — overlaps; cite as related but not
  necessarily closed.
- No PRs.

## Files touched

- `src/libstore/local-store.cc` — the read-and-hash loop in
  `verifyPath` / `repairPath`. Search for `readFile(realPath)` and the
  hashing block.
- `src/libutil/file-system.cc` — `readFile`/`readFileFD` may need an
  `EIO` → `Error("path %s is unreadable; treating as corrupted")`
  translation, or callers catch `SysError` with `errno == EIO` and
  remap.
- `src/libstore/gc.cc` (if `--verify` lives there).
- `tests/nixos/repair-eio.nix` — new NixOS test using `dm-flakey` or
  `dm-error` to inject `EIO` at the block layer underneath a known
  store path; assert `nix-store --repair-path` succeeds (re-fetches).
- `doc/manual/rl-next/repair-eio.md` — release note.

## Commits (in order)

1. **`libstore: classify EIO during repair as "path is corrupted, needs substitute"`**
   - In `LocalStore::repairPath` (or wherever the read happens), wrap the read in a `try/catch SysError` that checks `errno == EIO`. On hit, log `warn("path %s read failed with EIO; treating as corrupted and re-fetching", path)` and proceed as if the hash had mismatched.
   - Behaviour for paths *with no substitute available*: surface the original `EIO` with a clear "no substitute available; cannot repair" message, instead of the bare "Input/output error" today.

2. **`libstore: classify EIO during --verify --check-contents the same way`**
   - Same wrap in `verifyAllValidPaths` / `verifyPath`. The `--repair` flag already controls whether to attempt re-fetch; on `EIO` without `--repair`, mark the path as invalid and continue.

3. **`tests: NixOS test for EIO-on-read repair flow`**
   - Use `dm-flakey` to make a small block device that randomly returns
     `EIO`. Place a known store path on it. Run
     `nix-store --repair-path /nix/store/...-foo` and assert the path
     is re-fetched from a binary cache (or a local substituter set up
     by the test).

4. **`doc: release note`**
   - "`nix-store --repair-path` now correctly handles paths whose
     contents are unreadable due to filesystem-level corruption (ZFS
     scrub, btrfs csum mismatch, dm-integrity error). Previously
     bailed; now re-fetches from a substituter (closes #8121, #2735)."

## Tests

- NixOS test as in commit 3.
- Unit: extend an existing `LocalStore` test to inject a fake source
  accessor that throws `EIO` from `readFile`; assert repair flow is
  triggered.
- Manual: on a real ZFS pool with a known scrub-detected corrupted
  path, run `nix-store --repair-path /nix/store/...` and verify
  re-fetch.

## Risk and rollback

Risk: low.

- The classification could mis-attribute *transient* `EIO` (USB drive
  briefly disconnected) as permanent corruption. Mitigation: log
  loudly; the user can always disable `--repair` and just see the
  warning. The substitute fetch will fail-fast if the disk is also
  un-writable.
- A binary cache might not have the path (e.g. CA-store with a custom
  derivation). In that case, the user gets a clearer error than
  before; no regression.

Rollback: trivial revert of any commit. The current behaviour is
"bail on EIO" — easy to restore.

## Dependencies

- None.
- Independent of plans 01-06, 08.

## Out of scope

- Auto-scheduling `nix-store --repair-path` after a ZFS scrub finishes
  (mentioned in #8121 as a "bonus points" feature). That's a NixOS
  module feature, not a Nix change. Track separately.

## Definition of done

- [ ] All commits land in order.
- [ ] NixOS test green.
- [ ] Issues #8121 and #2735 closed.
- [ ] Release note merged.
- [ ] Verified by hand on a ZFS pool with a known-corrupted store path.
