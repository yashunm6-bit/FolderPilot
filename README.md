# FolderPilot

A safe, Windows-first file organization utility written in modern C++.

FolderPilot will sort the files of a user-selected directory into category
folders (Documents, Images, Videos, Audio, Archives, Code, Other) — showing a
dry-run plan by default and moving files only on an explicit `--apply`.

## Status: Phase 6 — hardening, tests and documentation (complete, formally accepted)

Phases 1–3 delivered the build/test foundation, the classification core,
PathGuard and the handle-anchored scanner; Phase 4 added planning and
read-only dry-run previews; Phase 5 added the safe executor (`--apply`):
root/destination/source revalidation and handle-based, OS-enforced
no-overwrite movement. The anchored rename is performed with the native
`NtSetInformationFile(FileRenameInformation)` form (the Win32 class-3 form
fails with ERROR_INVALID_PARAMETER on build 26300 — a build-specific
observation, not a universal Windows contract; no path-based fallback
exists by design — see `docs/ARCHITECTURE.md` §11).
Runtime verification of the current Phase 6 state:
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
folderpilot <directory>            preview the organization plan (dry run, default)
folderpilot <directory> --apply    execute the planned movements
folderpilot --help | --version
```

- **Dry run is the default and never modifies anything.** Movement happens
  only with the explicit `--apply` flag. There is no `--force`, no undo,
  and no background mode.
- Exit codes: `0` dry-run report printed / all moves completed · `1` usage
  error · `2` safety rejection (target refused, root changed, invalid plan)
  · `3` scan or planning failure / execution completed with failures.
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
