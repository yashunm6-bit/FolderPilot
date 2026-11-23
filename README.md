# FolderPilot

A safe, Windows-first file organization utility written in modern C++.

FolderPilot will sort the files of a user-selected directory into category
folders (Documents, Images, Videos, Audio, Archives, Code, Other) — showing a
dry-run plan by default and moving files only on an explicit `--apply`.

## Status: Phase 8 — Recursive Organization & Retry (locally verified; Phases 1–7 complete and accepted)

Phases 1–3 delivered the build/test foundation, the classification core,
PathGuard and the handle-anchored scanner; Phase 4 added planning and
read-only dry-run previews; Phase 5 added the safe executor (`--apply`):
root/destination/source revalidation and handle-based, OS-enforced
no-overwrite movement. The anchored rename is performed with the native
`NtSetInformationFile(FileRenameInformation)` form (the Win32 class-3 form
fails with ERROR_INVALID_PARAMETER on build 26300 — a build-specific
observation, not a universal Windows contract; no path-based fallback
exists by design — see `docs/ARCHITECTURE.md` §11).
Runtime verification of the Phase 6 state (superseded):
**Local (this machine, authorized Smart App Control–off window): Debug
144/144 test cases, 1,762/1,762 assertions; Release 144/144 test cases,
1,762/1,762 assertions — 0 failed and 0 skipped in both.**
**Hosted (GitHub Actions, `yashunm6-bit/FolderPilot`)**: latest run
`43068578754` ("FolderPilot CI", push to `main`, commit `4fd157d`,
`windows-latest` runner with MSYS2 UCRT64) — workflow conclusion `success`;
Debug and Release Configure/Build/Test steps all succeeded, as did the
test-log verification and artifact-upload steps. It created the
`folderpilot-test-logs` artifact (ID `13876723467`, 1,272 bytes, not
expired; artifact metadata SHA matches commit `4fd157d`). Artifact
existence and metadata are verified; its contents could not be inspected
anonymously (HTTP 401), so exact hosted doctest counts are not claimed.
(Earlier run `43052752854`, commit `7f4a9db`, also succeeded but uploaded
no artifact — wrong CTest log path, since fixed. History:
`docs/PHASE6-CLOSEOUT.md` §3–§4.)
Runtime verification of the Phase 7.1c state (superseded; commit `e98089f`,
storage adapter + ratified Decision A, 2026-11-18): **Local (this machine; Smart App
Control off by owner decision): Debug and Release each 225/225 test cases,
14,112/14,112 assertions — 0 failed, 0 skipped — including genuine
cross-process mutation-lock contention and crash-release child-process tests.**
**Hosted (GitHub Actions, run `44341177181`, CI #14): success in 1m 42s —
Debug and Release Configure/Build/Test all green; the `folderpilot-test-logs`
artifact's SHA-256 was verified against the downloaded bytes, and its two
`LastTest.log` files show the same 225/14,112 totals on the runner.** The
ratified decisions A–E (NULL/absent-DACL refusal, root-key formula, hidden/
system rule, operability ACL scope, lock_id derivation convention) are
recorded in `docs/PHASE7-DESIGN.md` §16.
Runtime verification of the current Phase 8 state (recursive organization +
retry): **Local (this machine; Smart App Control off by owner decision):
Debug and Release each 279/279 test cases, 14,893/14,893 assertions — 0
failed, 0 skipped, 0 compiler warnings.** Hosted CI verification follows the
push of this tree; the exact run ID and head SHA are recorded in the
completion manifest.
Historical, superseded by the figures above — Phase 5 state (commits
`3940886`, `d024112`): 142/142 test cases, 1,748/1,748 assertions.
Phase 6 adds Unicode CLI argument handling, Windows hardening, packaging
documentation, and CI-based regression testing (complete).

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — approved design, and which
  parts are implemented vs. planned.
- [docs/SAFETY.md](docs/SAFETY.md) — safety invariants and their
  implementation status.

## Usage

```
folderpilot <directory>                       preview the organization plan (dry run, default)
folderpilot <directory> --apply               execute the planned movements (journaled)
folderpilot <directory> --undo                preview the undo plan (dry run)
folderpilot <directory> --undo --apply        restore journaled files
folderpilot <directory> --recursive [--apply] include subdirectories (forward only)
folderpilot <directory> --retry [--apply]     re-attempt locked/denied moves
folderpilot <directory> --apply --acknowledge-undo-window
folderpilot <directory> --retry --apply --acknowledge-undo-window
folderpilot --help | --version
```

`--recursive` is forward-only and cannot be combined with `--undo` or `--retry`;
`--retry` is likewise mutually exclusive with both. The acknowledgement is
valid only with an actual forward execution (`--apply` or `--retry --apply`).

- **Dry run is the default and never modifies anything.** Movement happens
  only with the explicit `--apply` flag. There is no `--force` and no
  background mode.
- **Undo (Phase 7)**: every `--apply` run is journaled; `--undo` previews
  what would be restored (read-only), and `--undo --apply` moves the
  journaled files back to their original locations (never overwriting — a
  refilled slot is a reported Conflict). A live undo window refuses further
  forward applies until superseded via `--apply --acknowledge-undo-window`
  (durably recorded). The acknowledgement is also valid with `--retry
  --apply`; it is a usage error (exit 1) with a dry run, `--undo` or
  `--retry` alone. Undo restores locations, never file contents.
- **Recursive organization (Phase 8)**: `--recursive` also organizes files
  in subdirectories (deterministic order; whole-plan collision suffixes
  across depths). Reparse points (junctions/symlinks), hidden, and system
  directories are never traversed, and the created category directories
  are never scanned as sources — the traversal cannot escape the target
  directory. Journaled and undoable like every other run; `--undo --apply`
  restores nested files to their original nested locations (directories
  are never created — a removed intermediate is a reported failure).
- **Retry (Phase 8)**: `--retry` lists the locked/denied moves of the
  newest journaled run and re-checks each file's fingerprint; `--retry
  --apply` re-attempts exactly the files whose recorded location and
  fingerprint still match, as a fresh journaled run (the original journal
  is never rewritten). Moved, permanent, identity-changed, and generic `Failed`
  outcomes are never retried. A blocked or busy journal exits `2`; if nothing
  is retryable the run exits `0` with a disclosure; a retry that finishes with
  any failures exits `3`. With `--retry --apply`, a full recursive re-scan
  fails closed on any inaccessible subdirectory (exit `3`).
- Exit codes: `0` dry-run report printed / all moves completed · `1` usage
  error · `2` safety rejection (target refused, root changed, invalid plan)
  or blocked/busy journal · `3` scan or planning failure, operational journal
  storage I/O failure, or execution completed with failures.
- Directory arguments are received through the UTF-16 entry point and
  converted with strict UTF-16→UTF-8 conversion: non-ASCII paths (Unicode
  folder names) round-trip exactly. Console output is UTF-8.
- Safety summary: targets are validated by handle-based PathGuard
  (attribute gates for reparse points, hidden/system/hidden directories;
  drive roots, UNC, and system-subtree denylist); every movement is a
  handle-anchored rename with `ReplaceIfExists = FALSE` — the OS refuses
  to overwrite an existing destination; nothing outside the validated
  target is ever touched. See `docs/SAFETY.md`.

## Building (MSYS2 UCRT64 shell)

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Outputs: `build/debug/folderpilot.exe` (application) and
`build/debug/folderpilot_tests.exe` (test suite). The same three commands
work with `--preset release`.

Observed on one development machine: action-performing CMake preset
invocations (`cmake --preset <name>`, `cmake --build --preset <name>`) exit
127 silently. The cause remains unidentified and is not attributed to any
security policy without evidence; preset usage errors and `ctest --preset`
listing work on the same machine. The direct CMake forms below are
equivalent for a fresh tree (note the explicit build types):
`cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug`,
`cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release`,
then `ninja` inside the build directory and `ctest` from it.

## Packaging and distribution

- Both executables link the GCC runtime statically (`-static`): a release
  `folderpilot.exe` is a single self-contained file with no PATH-dependent
  DLL lookups. Distribution is copying the Release executable.
- An install rule is provided: `cmake --install build/release --prefix <dir>`
  copies `folderpilot.exe` into `<dir>` (default `bin/`).
- The application is unsigned; see the next section for what that means
  under Smart App Control.

## Regression testing under Smart App Control

Smart App Control (SAC) blocks freshly linked, unsigned executables at
launch — including this project's own test suite — and re-linking produces
a new file each time. Do **not** disable SAC to run tests. The supported
paths are:

1. **Hosted CI** (preferred): `.github/workflows/ci.yml` builds both
   configurations and runs the full suites on a Windows runner, where the
   freshly built binaries execute normally. The workflow runs only when
   pushed/triggered; it is never triggered from a local machine. Hosted
   runs (2026-11-11): `43052752854` (push of `7f4a9db`) succeeded but its
   log artifact was never uploaded — wrong CTest log path, since fixed in
   `4fd157d`; `43068578754` (push of `4fd157d`) succeeded and created the
   `folderpilot-test-logs` artifact, verified against that commit. Evidence
   and limits: `docs/PHASE6-CLOSEOUT.md` §3–§4 (artifact contents are not
   anonymously readable, so hosted test counts are not quoted).
2. **Machines without SAC enforcement** (e.g. SAC evaluation mode or a
   lab machine with a documented exception): build and run `ctest --preset debug`
   and `ctest --preset release` locally.

## Layout

| Directory | Contents |
|---|---|
| `app/` | CLI entry point |
| `lib/folderpilot/` | Core library: classification, PathGuard, scanner (Phases 2–3) |
| `platform/` | Windows platform layer: native file handling, text conversion (Phases 2–3) |
| `tests/` | doctest unit and integration tests |
| `third_party/doctest/` | Vendored doctest (pinned release + upstream licence) |
| `docs/` | Architecture and safety documentation |

## Toolchain

- C++20 (compiler-specific extensions disabled)
- GCC via MSYS2 UCRT64
- CMake (>= 3.21) + Ninja, driven by committed presets
- doctest, vendored and pinned (see `docs/ARCHITECTURE.md`)
