# FolderPilot

A safe, Windows-first file organization utility written in modern C++.

FolderPilot will sort the files of a user-selected directory into category
folders (Documents, Images, Videos, Audio, Archives, Code, Other) — showing a
dry-run plan by default and moving files only on an explicit `--apply`.

## Status: Phase 5 — safe executor (movement blocked on OS build 26300)

Phases 1-3 delivered the build/test foundation, the classification core,
PathGuard and the handle-anchored scanner; Phase 4 added planning and
read-only dry-run previews; Phase 5 added the safe executor (`--apply`):
root/destination/source revalidation and handle-based, OS-enforced
no-overwrite movement.
**KNOWN BLOCKER: on Windows build 26300 the RootDirectory-relative rename
form fails with ERROR_INVALID_PARAMETER, so movements currently fail closed
on this build (sources untouched). No path-based fallback exists by design.**

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — approved design, and which
  parts are implemented vs. planned.
- [docs/SAFETY.md](docs/SAFETY.md) — safety invariants (all marked
  PLANNED — NOT YET IMPLEMENTED).

## Building (MSYS2 UCRT64 shell)

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Outputs: `build/debug/folderpilot.exe` (application) and
`build/debug/folderpilot_tests.exe` (test suite). The same three commands
work with `--preset release`.

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
