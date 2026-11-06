# FolderPilot

A safe, Windows-first file organization utility written in modern C++.

FolderPilot will sort the files of a user-selected directory into category
folders (Documents, Images, Videos, Audio, Archives, Code, Other) — showing a
dry-run plan by default and moving files only on an explicit `--apply`.

## Status: Phase 3 — safe target validation & scanning

Phase 1 delivered the build/test foundation; Phase 2 added the pure
in-memory classification core and strict UTF-8/UTF-16 conversion; Phase 3
added PathGuard (target validation) and a handle-anchored, non-recursive
directory scanner — strictly read-only.
**FolderPilot cannot plan, organize, or move any files yet.**

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
