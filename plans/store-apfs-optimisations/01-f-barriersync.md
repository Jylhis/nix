# 01 — `F_BARRIERFSYNC` for store-path data on macOS

## Goal

Replace the unconditional `fcntl(F_FULLFSYNC)` in `syncDescriptor()` with `F_BARRIERFSYNC` for the store-path code path, keeping `F_FULLFSYNC` for the SQLite database (where SQLite already manages its own fsync strategy and the user-set `fsync-metadata` semantics promise full durability).

## Why

`F_FULLFSYNC` (`src/libutil/unix/file-descriptor.cc:147`) asks the drive to flush its battery-backed cache. On consumer SSDs that's roughly an order of magnitude slower than `F_BARRIERFSYNC`, which only guarantees ordering ("barrier semantics" — writes issued before this point reach stable storage before any issued after). For Nix store-path registration the actual contract we need is "file contents must be durable before the SQLite row marking the path valid commits"; that is precisely an ordering contract.

Background: SQLite, Postgres and Firefox all moved to `F_BARRIERFSYNC` for non-WAL paths, with documented evidence on consumer SSDs.

## Existing related upstream work

- None — `gh search` and `git log` find no prior touch of `F_BARRIERFSYNC` or `syncDescriptor` apart from initial introduction.
- The `fsync-store-paths` setting (`src/libstore/include/nix/store/local-settings.hh:222-230`) is the existing user-visible knob that gates whether this code path runs at all, so users who care about robustness can A/B against the current behaviour.

## Files touched

- `src/libutil/unix/file-descriptor.cc` — primary change.
- `src/libutil/include/nix/util/file-descriptor.hh` — extend the `syncDescriptor()` signature with a strength enum (or add a sibling).
- `src/libutil/file-system.cc` — call sites in `syncParent()` / `recursiveSync()` pick the new strength.
- `src/libstore/local-store.cc` — SQLite `fsync` paths must keep `F_FULLFSYNC` semantics; verify call sites and pin them explicitly.
- `doc/manual/rl-next/f-barriersync.md` — release note.

Do **not** touch: SQLite, anything under `src/libstore/sqlite.cc` — let the existing strong-fsync stay for the database file.

## Commits (in order)

1. **`libutil: introduce FsyncStrength enum on syncDescriptor`**
   - Add `enum class FsyncStrength { Barrier, Full }` (Barrier = default).
   - Overload / add a parameter; on non-`__APPLE__` platforms both map to `fsync()`.
   - On `__APPLE__`, `Full` → `F_FULLFSYNC`, `Barrier` → `F_BARRIERFSYNC` if available, else fall back to `F_FULLFSYNC` (see availability note below).
   - Update every existing call site to pass `Full` so behaviour is byte-identical at this commit. Build & tests pass unchanged.

2. **`libstore: use barrier fsync for store-path data`**
   - In `RestoreRegularFile::~RestoreRegularFile` (via `AutoCloseFD::fsync()`), `recursiveSync()`, and `syncParent()`, pass `FsyncStrength::Barrier`.
   - Leave SQLite code paths alone (verify by grep — there should be no `syncDescriptor` calls under `src/libstore/sqlite*`).
   - Add a one-line comment at each call site explaining why ordering suffices.

3. **`doc: add release note for barrier fsync on macOS`**
   - `doc/manual/rl-next/f-barriersync.md` — three to five lines: what changed, what users notice (faster `fsync-store-paths = true`, no behaviour change on Linux/FreeBSD/Windows), pointer to the original Apple WWDC 2018 talk on `F_BARRIERFSYNC`.

4. **`tests: cover FsyncStrength fallback path`**
   - `src/libutil-tests/file-descriptor.cc` — gated on `__APPLE__`; opens a temp file, calls both strengths, asserts no error. The point isn't to verify on-disk durability (we can't from userspace) but to lock the build down so the fallback compiles on every supported macOS deployment target.

## Availability

`F_BARRIERFSYNC` is documented since macOS 11 (Big Sur, 2020). Nixpkgs supports back to macOS 10.13 for `x86_64-darwin`.

Two acceptable strategies:

- **Compile-time**: detect via `<sys/fcntl.h>` having `F_BARRIERFSYNC`. The fcntl numeric constant exists in headers since Xcode 12; if missing, fall through to `F_FULLFSYNC`.
- **Runtime**: call `fcntl(fd, F_BARRIERFSYNC)`; if it returns `EINVAL`, set a `std::atomic<bool>` flag and from then on use `F_FULLFSYNC`.

Pick **runtime** — it's one branch, the branch is well-predicted after the first call, and it keeps Nixpkgs builds buildable against the older SDKs without conditional compilation.

## Tests

- Unit test as in commit 4.
- Functional: `tests/functional/build.sh` already runs with `fsync-store-paths` toggled. Add an explicit `nix-build … --option fsync-store-paths true` invocation and ensure no regressions.
- Benchmark in PR description: `time nix copy --to ./store /nix/store/<some-large-closure>` with `fsync-store-paths = true`, before vs after, on an APFS-formatted external SSD. Cite the disk model.

## Risk and rollback

Risk: very low. If `F_BARRIERFSYNC` were silently weaker than the kernel claims, store paths could become visible before contents are durable across a power loss. Mitigations:

- Keep `F_FULLFSYNC` for SQLite (the registration commit) — this means even in the worst case the registration row only commits *after* its barrier-fsynced contents, which is equivalent to the original behaviour modulo Apple's barrier promise.
- Hide the change behind no new setting; users who distrust it can rebuild with `-DNIX_FORCE_FULLFSYNC` or set the existing `fsync-store-paths = false` (they already accept "no fsync" today).

Rollback: revert the two `libstore` and `libutil` commits — the enum stays in place, every call defaults to Full. No on-disk format change.

## Out of scope

- Adding asynchronous fsync (covered by plan 02).
- Changing SQLite fsync strategy.
- Replacing the `fsync-metadata` setting semantics.

## Definition of done

- [ ] Both unit and functional tests green on Linux and macOS CI.
- [ ] Benchmark numbers in PR description.
- [ ] Release note merged.
- [ ] No reviewer-flagged regressions on the `fsync-metadata` / `fsync-store-paths` combos.
