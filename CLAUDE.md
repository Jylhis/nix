# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository

This is the source for **Nix**, the purely functional package manager (C++20, Meson build, LGPL v2.1). Upstream: <https://github.com/NixOS/nix>. The user-facing manual lives under `doc/manual/source/`; the development sub-tree (`doc/manual/source/development/`) is the authoritative reference for building, testing, debugging, and style — prefer it over inferring from code.

## Development environment

All work happens inside a Nix dev shell that provides the full toolchain (clang/gcc, meson, ninja, clangd, pre-commit, etc.) and exposes Nixpkgs build phases as shell functions:

```sh
nix develop                      # default stdenv
nix develop .#native-clangStdenv # clang + clangd; preferred for editor LSP
nix develop .#native-ccacheStdenv  # ccache for faster rebuilds
```

Inside the shell, the build is driven through the Nixpkgs phase functions, not raw meson:

```sh
configurePhase   # runs meson setup into ./build (mesonBuildDir)
buildPhase       # ninja -C build
checkPhase       # runs meson test (unit + functional)
installPhase     # installs into ./outputs/
```

`nix develop` adds `./outputs/bin/nix` to `PATH` so `nix --version` works after `installPhase`. Set `mesonBuildDir=build-<variant>` before `configurePhase` to keep multiple build directories (e.g. for cross-builds) against one source tree.

For sanitizer builds, edit `mesonFlags` (see `doc/manual/source/development/debugging.md`) — Boehm GC and Perl bindings must be disabled for ASan.

## Building, testing, formatting

- **Build everything once configured:** `meson compile -C build` (or `ninja -C build`).
- **Run all tests:** `meson test -C build` or `checkPhase`.
- **Run one functional test:** `meson test -C build --verbose <testName>` (e.g. `add`, `build`, `flakes/show`). Functional tests are bash scripts under `tests/functional/` and can be invoked directly: `TEST_NAME=add NIX_REMOTE='' tests/functional/add.sh`.
- **Run a test group/suite:** `meson test -C build --suite ca` (suites are defined per-subdirectory `meson.build`, e.g. `ca`, `flakes`, `lang`).
- **Run one unit-test executable:** `meson test -C build nix-expr-tests`. Filter inside it with googletest envvars: `GTEST_BRIEF=1 GTEST_FILTER='ErrorTraceTest.*' meson test -C build nix-expr-tests -v`.
- **Debug a failing test:** `meson test -C build <name> --gdb` (unit) or `--interactive` (functional, lets you drop into gdb mid-script).
- **Regenerate characterisation/“golden master” outputs:** `_NIX_TEST_ACCEPT=1 meson test -C build <name>` — the test will mark itself SKIPPED while it rewrites expected files. Used by the language tests (`tests/functional/lang/`) and the worker-protocol/json characterisation tests in `src/*-tests/`.
- **Clean stray artefacts left in the source tree by tests:** `git clean -x --force tests`.
- **Format:** `nix develop -c ./maintainers/format.sh` (wraps `pre-commit run --all-files`). Install the hook once with `pre-commit-hooks-install` from inside the dev shell. C++ uses `.clang-format` (LLVM base, 4-space indent, 120 cols, `PointerAlignment: Middle`, `SortIncludes: Never`); Nix files use `nixfmt`.
- **Static analysis:** `meson compile -C build clang-tidy` (warnings are CI-fatal). Auto-apply fixes with `meson compile -C build clang-tidy-fix`, then review the diff. Config is `.clang-tidy` (symlinked from `nix-meson-build-support/common/clang-tidy/.clang-tidy`); custom Nix-specific checks live in `src/clang-tidy-plugin/` and use the `nix-` prefix.
- **Integration / VM / NixOS tests** are flake jobs, not part of `meson test`: `nix build .#hydraJobs.tests.<name>` (definitions in `tests/nixos/` and the `hydraJobs.tests` flake attribute).

## Code architecture

Nix is split into a stack of internal C++ libraries plus a thin C ABI wrapper for each, tested out-of-tree, and consumed by a single multi-call CLI. Layering (lower → higher; each layer may only depend on lower ones):

1. `libutil` — generic utilities (strings, JSON, archive/NAR, async, logging, args parsing).
2. `libstore` — the store abstraction (local store, daemon protocol, binary caches, build engine, derivations, content addressing). Builders/sandboxing live under `src/libstore/.../build/`.
3. `libfetchers` — pluggable source fetchers (git, github, tarball, mercurial, …).
4. `libexpr` — the Nix expression language: lexer/parser, AST (`nixexpr.hh`), evaluator (`eval.hh`, `eval-inline.hh`), value representation, primops, eval cache, GC integration (Boehm).
5. `libflake` — flakes on top of libexpr + libfetchers (lock-file resolution, flake schema).
6. `libmain` — shared CLI plumbing (logger setup, signal handling, `initNix`).
7. `libcmd` — high-level command framework (`Command`, `MixEvalArgs`, installable resolution).

Each internal library `libfoo` has companions:

- `libfoo-c/` — a stable C ABI wrapper (`nix_*` functions); these are the public API for embedders.
- `libfoo-tests/` — googletest + rapidcheck unit tests, with test data under `libfoo-tests/data/` exposed to the binary via `_NIX_TEST_UNIT_DATA`.
- `libfoo-test-support/` — shared `Arbitrary` instances and helpers reused by *downstream* libraries' property tests. Must contain no tests itself.

Other top-level source areas:

- `src/nix/` — the unified `nix` CLI plus the legacy entry points (`nix-build`, `nix-env`, `nix-store`, `nix-instantiate`, `nix-channel`, `nix-collect-garbage`, `nix-copy-closure`, `build-remote`). All are subcommands of one binary, dispatched by argv[0].
- `src/perl/` — Perl bindings (`bindings` meson option; auto-disabled under ASan and cross-builds).
- `src/clang-tidy-plugin/` — custom AST-matchers for project-specific lints; built only if LLVM dev headers are available.
- `src/json-schema-checks/` — validates the JSON test fixtures against the JSON Schemas published in the manual, so manual + impl + schema stay in sync.
- `src/external-api-docs/`, `src/internal-api-docs/`, `src/nix-manual/` — Doxygen + mdBook outputs, gated by the `doc-gen` meson option.
- `nix-meson-build-support/` — shared meson snippets and the canonical `.clang-tidy`/`.clang-format` configs that the root files symlink to.
- `tests/functional/` — bash-driven black-box tests of the built binary; subdirs (`ca`, `flakes`, `lang`, `git`, `dyn-drv`, …) double as meson suites.
- `tests/nixos/` — NixOS VM tests for things that need a real system (sandboxing, daemon over TCP, fetchers against live services, etc.); runnable only via the flake.

### Header / impl conventions

- Public headers live in `src/<lib>/include/nix/<lib>/...` and are included as `#include "nix/<lib>/foo.hh"`. Source files (`.cc`) sit directly under `src/<lib>/`. Tests mirror the layout under `src/<lib>-tests/` and `src/<lib>-test-support/include/nix/<lib>/tests/`.
- Templates use the **`*-impl.hh` pattern** documented in `doc/manual/source/development/cxx.md`: declarations (including template signatures) go in `foo.hh`; template *definitions* go in `foo-impl.hh`, which `foo.hh` deliberately does **not** include. Translation units that instantiate a template must include both headers explicitly. This is intentional — it keeps `foo.hh` lightweight for callers that only use non-template declarations.
- `SortIncludes: Never` is set in `.clang-format` because include order is sometimes load-bearing — preserve the existing order when editing.

## Pull-request expectations

The CONTRIBUTING.md checklist is enforced in review:

- Tests at the appropriate layer: functional (`tests/functional/*.sh`), unit (`src/*-tests/`), or integration (`tests/nixos/*`).
- User-facing manual updates in `doc/manual/source/`. Use semantic line breaks ([sembr](https://sembr.org)) in markdown.
- API docs in headers (Doxygen-style).
- Commit messages must explain **why**, not just what.
- New features or any incompatible change require a release note (see `doc/manual/source/development/contributing.md` for the `doc/manual/rl-next/` workflow).
- Keep history clean by rebasing on `master`; do not merge `master` into the branch.

CI runs the full `meson test`, the clang-tidy job (warnings = errors), the manual build, JSON-schema checks, and the NixOS VM test matrix on every PR.
