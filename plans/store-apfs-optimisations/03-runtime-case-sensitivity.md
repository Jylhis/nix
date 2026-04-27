# 03 — Runtime case-sensitivity probe for `use-case-hack`

## Goal

Stop forcing `use-case-hack = true` for every `__APPLE__` build (`src/libutil/archive.cc:18-26`). Probe the underlying volume at startup; default `true` only when the store volume is actually case-insensitive. The user-set value still wins.

## Why

The case-hack suffixes filenames like `README.md` and `readme.md` with `~nix~case~hack~` to make collisions representable on case-insensitive filesystems. The macOS installer (`scripts/create-darwin-volume.sh:64-65`) lets users pick `Case-sensitive APFS`. On those volumes the case-hack is wasted work and a footgun: real distinct-case filenames get suffixed unnecessarily, and tarballs generated on a case-sensitive store carry the suffixes when they shouldn't.

Per [issue #10746](https://github.com/NixOS/nix/issues/10746) discussion (Lily Ballard, 2025), there is no in-place APFS conversion from case-insensitive to case-sensitive — users who want this had to recreate `/nix`. They are exactly the population that the current default penalises.

## Existing related upstream work

- [Issue #10746](https://github.com/NixOS/nix/issues/10746) "Darwin installer: create a case sensitive APFS volume" (open Sep 2025) — substantial discussion on installer changes, `/tmp`, `build-dir`, Meson edge cases, and Windows parallels (NTFS per-directory case-sensitive xattr). This plan implements the **runtime probe half**; the **installer half** stays out of scope and remains tracked there.
- [Issue #2415](https://github.com/NixOS/nix/issues/2415) "Set macOS/kernel to be case sensitive in builder" — tangentially related, reports build-time issues from the case-hack.
- No PRs propose a runtime probe.

## Files touched

- `src/libutil/archive.cc` — change the default initialiser.
- `src/libutil/include/nix/util/file-system.hh` + `src/libutil/unix/file-system.cc` — add `bool isCaseSensitiveFilesystem(const std::filesystem::path &)`.
- `src/libstore/local-store.cc` — at store init, log a one-line `info` message stating which mode was selected for the store directory.
- `src/libutil-tests/file-system.cc` — unit tests using `tmpfile` and a mocked accessor.
- `doc/manual/source/command-ref/conf-file.md` (or wherever `use-case-hack` is documented) — note the new auto-detect behaviour.
- `doc/manual/rl-next/case-sensitivity-probe.md` — release note.

## Commits (in order)

1. **`libutil: add isCaseSensitiveFilesystem(path) probe`**
   - Linux/FreeBSD: `pathconf(path, _PC_CASE_SENSITIVE)` if defined, else default `true`.
   - macOS: `getattrlist` with `ATTR_VOL_CAPABILITIES`, check `VOL_CAP_FMT_CASE_SENSITIVE` in the format-capabilities mask. Cache result by `st_dev` (one entry suffices in practice — the store lives on one volume).
   - Windows: WSL-style per-directory case-sensitivity attribute query (out of scope here; return `true` until someone asks).
   - Errors / unsupported syscalls → return `std::nullopt` (caller decides default).

2. **`libutil: switch use-case-hack default to runtime probe`**
   - In `archive.cc`, change the default to a thunk that probes the store path on first read.
   - Setting can still be overridden in `nix.conf`; explicit value wins.
   - Default fallback when the probe fails: keep the current `__APPLE__` → `true` behaviour for safety. Log at `lvlTalkative` so it's visible under `--verbose` but not noisy.

3. **`libstore: log selected case-hack mode at store init`**
   - One-line `info` line on first use, e.g. `"using case-hack mode for /nix/store (case-insensitive APFS volume detected)"`. Helps users understand surprising behaviour in tarballs etc.

4. **`tests: cover case-sensitivity probe`**
   - Unit test: create temp dirs, query, assert. macOS-only assertion checks the typed path.
   - Functional: add an explicit `nix-store --add` test that creates files differing only in case, runs once with `use-case-hack = true` and once with `false`, asserts both behave correctly. (This may already exist; extend it.)

5. **`doc: document the runtime probe and migration story`**
   - Release note: behaviour change for case-sensitive APFS users (no more spurious `~nix~case~hack~` suffixes in their tarballs); behaviour for case-insensitive users unchanged.
   - Update the manual entry for `use-case-hack` to describe the auto-detect default.

## Compatibility / migration

The case-hack suffix is *encoded into NAR* — flipping the setting on a store that previously hashed paths with the suffix changes their content hash. So:

- **Case-insensitive APFS users (the historical default):** no behaviour change (probe returns `false`, hack stays on).
- **Case-sensitive APFS users:** historically had to manually set `use-case-hack = false` and lived with paths possibly containing the suffix. After this change the probe sees their volume and defaults to `false`. Their existing CA-store paths with the old suffix still hash correctly because the suffix is preserved in the NAR. New tarballs / new hashes are produced without the suffix — same as if they had set the option themselves.
- **Mixed builds (build-dir on case-insensitive `/tmp`, store on case-sensitive `/nix`):** noted in issue #10746 as the hard problem. This plan doesn't solve it; we probe the **store path**, not the build directory. Add a TODO comment pointing at issue #10746.

## Risk and rollback

Risk: medium-low. The setting is on a hot path (every NAR serialisation reads `useCaseHack`), so the probe must be cached after first call.

Rollback: revert commit 2; the probe and tests remain (no harm).

## Dependencies

- None.

## Out of scope

- Installer changes (issue #10746).
- `build-dir` case-sensitivity (issue #10746).
- Windows per-directory case-sensitive attribute (separate plan if requested).

## Definition of done

- [ ] Unit + functional tests green.
- [ ] Manual updated.
- [ ] Release note merged.
- [ ] Probe result cached per `st_dev`.
- [ ] Verified by hand: `use-case-hack` set explicitly still wins on both case-sensitive and case-insensitive volumes.
