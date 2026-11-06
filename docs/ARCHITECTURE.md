# FolderPilot — Architecture

> **Implementation status: Phase 2 (classification core) complete.**
> This document records the architecture approved in Engineering Plan v2.5
> (plus the ratified directory-enumeration mechanism). Only items marked
> **IMPLEMENTED** exist in code today. Everything else is **PLANNED — NOT YET
> IMPLEMENTED**. FolderPilot can classify filenames in memory only; it still
> cannot scan directories, organize, or move any files.

---

## 1. Implementation status

| Area | Status |
|---|---|
| Build system: CMake ≥ 3.21, Ninja, presets, CTest | **IMPLEMENTED** (Phase 1) |
| Test foundation: vendored doctest, smoke test | **IMPLEMENTED** (Phase 1) |
| Application entry point (scaffold only) | **IMPLEMENTED** (Phase 1) |
| Core types: `Category`, stable display names | **IMPLEMENTED** (Phase 2) |
| Extension rules: sorted static table, binary-search lookup | **IMPLEMENTED** (Phase 2) |
| Pure filename classification (no filesystem access) | **IMPLEMENTED** (Phase 2) |
| Platform text utilities: strict UTF-8 / UTF-16 conversion | **IMPLEMENTED** (Phase 2) |
| Scanner, PathGuard, planner, reporter, executor | PLANNED — NOT YET IMPLEMENTED (Phases 3–5) |
| Platform layer: attribute inspection, NtCreateFile relative opens, handle-based movement | PLANNED — NOT YET IMPLEMENTED (Phases 3, 5) |
| Dry-run reporter, `--apply` executor | PLANNED — NOT YET IMPLEMENTED (Phases 4–5) |
| Any file organization behavior | PLANNED — NOT YET IMPLEMENTED |

## 2. Core model (approved)

**Plan → validate → execute.**

1. The **planner** produces an exact, immutable list of `{source →
   destination}` moves (including any collision suffixes), plus skip reasons.
   After planning, the plan is never mutated.
2. The **reporter** renders exactly that plan (dry-run output).
3. The **executor** follows exactly that plan, move for move. It never
   re-classifies, never re-resolves collisions, and never generates
   replacement destination names at execution time.
4. **Dry-run is the default.** Mutation requires the explicit `--apply`
   option. Dry-run and `--apply` are separate invocations; for an unchanged
   filesystem and unchanged inputs, planning is deterministic and the runs
   produce equivalent results.
5. A destination that exists at execution time fails that individual move
   safely (`Conflict`): source untouched, destination content unchanged,
   other independent moves continue.

## 3. Modules (approved layout)

```
app/               CLI entry point; argument parsing, exit codes, output
lib/folderpilot/   Core library (portable, unit-testable)
  - category / rules / classifier    extension → category (pure functions)
  - path_guard                       target validation, component-aware denylist
  - scanner                          directory listing + protection filters
  - planner / organization_plan      plan building, planning-time collision resolution
  - reporter                         plan rendering
  - executor                         plan execution (only via the platform layer)
platform/          Windows-specific seam (the only code touching Win32)
  - attributes        handle-based attribute/reparse inspection
  - nt_relopen        NtCreateFile opens/creates relative to the root handle
  - safe_move         handle-based movement (FILE_RENAME_INFO)
  - text / volume     UTF-8 ↔ UTF-16, volume-GUID identity
```

## 4. Approved Windows mechanisms (all PLANNED — NOT YET IMPLEMENTED)

| Concern | Mechanism |
|---|---|
| **Bootstrap open** | Exactly one path-based open of the user-selected root, with leaf-un-followed reparse inspection (`FILE_OPEN_REPARSE_POINT`), resolving the final path and root identity through that one handle |
| **Directory enumeration** | Handle-based enumeration via `GetFileInformationByHandleEx` with `FileIdBothDirectoryInfo` against the retained root handle |
| **All other opens/creates** | `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory` — sources and category directories are opened/created **relative to the retained root handle**; no absolute-path opens in the executor; no fallback to unsafe path-based opens |
| **Attribute inspection** | `FileAttributeTagInfo` through the opened handle: reparse point (any tag), hidden, system → protected entry, skipped/rejected with reason |
| **Source identity** | `FileIdInfo` (ULONGLONG volume serial + FILE_ID_128 file id) captured at plan time, re-verified pre-move; identity changes are skipped and reported, content changes are allowed |
| **Movement** | `SetFileInformationByHandle(FileRenameInfo)`: `ReplaceIfExists = FALSE`, `RootDirectory` = retained destination handle, relative filename; overwrite impossible, same-volume structural; **no path-based rename fallback exists** |
| **Handle sharing** | No `FILE_SHARE_DELETE` on retained root, destination, or source handles; pre-existing DELETE-capable handles cause a safe acquisition failure; concurrent renames/deletes are blocked during the execution window |
| **Access rights** | Requested at open, fail-closed: root `READ_ATTRIBUTES\|TRAVERSE\|ADD_SUBDIRECTORY\|SYNCHRONIZE`; destination `ADD_FILE\|READ_ATTRIBUTES\|TRAVERSE\|SYNCHRONIZE`; source `DELETE\|READ_ATTRIBUTES\|SYNCHRONIZE` |
| **Volume identity** | `GetVolumePathNameW` → normalized (exactly one trailing backslash) → `GetVolumeNameForVolumeMountPointW` → volume-GUID comparison only |
| **Destination categories** | Attribute-gated at planning **and** execution; reparse/hidden/system/non-directory categories are reported unsafe and receive zero moves |
| **Error taxonomy** | Only confirmed absence permits directory creation; access-denied, invalid path, and all other unresolved states fail closed |

Safety invariants and their wording constraints: see
[SAFETY.md](SAFETY.md).

## 5. Toolchain (IMPLEMENTED in Phase 1)

- **C++20**, compiler-specific extensions disabled (`-std=c++20`).
- **GCC** (MSYS2 UCRT64) — warnings `-Wall -Wextra -Wpedantic`; `-Werror`
  intentionally not enabled at this stage.
- **CMake ≥ 3.21 + Ninja** via committed `CMakePresets.json`
  (schema version 3; debug and release configure/build/test presets).
  No machine-specific compiler paths are committed.
- Targets: `folderpilot` (application), `folderpilot_tests` (doctest suite).

## 6. Testing (foundation IMPLEMENTED, suite PLANNED)

- Framework: **doctest**, vendored at
  `third_party/doctest/doctest.h`, pinned to upstream release
  **v2.5.3** (version macros verified: 2 / 5 / 3), with the upstream MIT
  licence preserved at `third_party/doctest/LICENSE.txt`.
- One test runner (`folderpilot_tests`); CTest entry `unit_tests` runs all
  registered doctest cases. Per-case discovery may be added as the suite
  grows.
- Phase 2 suites: `test_category`, `test_rules`, `test_classifier`,
  `test_text` (all purely in-memory), plus the Phase 1 smoke test.
- All future integration tests operate exclusively in isolated temporary
  directories; tests never target real user data. `remove_all` appears only
  in test-fixture cleanup, never in product code.

## 7. Phase map

| Phase | Scope | Status |
|---|---|---|
| 1 | Project scaffold and build system | **IMPLEMENTED** |
| 2 | Core types and classification | **IMPLEMENTED** |
| 3 | PathGuard and directory scanner | PLANNED |
| 4 | Planner and dry-run | PLANNED |
| 5 | Safe executor | PLANNED |
| 6 | Hardening, tests and documentation | PLANNED |

## 8. Phase 2 implementation notes (IMPLEMENTED)

- **Classification is pure.** `classify_filename(std::string_view)` performs
  no filesystem access of any kind — it is a deterministic string function,
  testable entirely in memory.
- **Case handling is ASCII-only and locale-free.** Only `'A'..'Z'` are
  folded; UTF-8 bytes (≥ 0x80) are never touched, so Unicode basenames pass
  through byte-exact. No locale-dependent `tolower` anywhere.
- **Compound archives** (`.tar.gz`, `.tar.bz2`, `.tar.xz`) are recognized
  before single-extension lookup. The extension after the LAST dot decides
  otherwise; dotfiles (`.gitignore`, `.pdf`), names ending in a dot, and
  unknown extensions map to `Other`.
- **Rule table**: 90 documented extensions across Documents (15), Images
  (14), Videos (11), Audio (11), Archives (11), Code (28) — stored as a
  `constexpr` sorted array, searched by ASCII case-insensitive binary
  search, `Other` is the fallback with no table entries.
- **Normative-conflict resolution (`.ts`)**: the Phase 2 rule lists contained
  `.ts` under both Videos and Code, while every extension must map to
  exactly one category. Resolved to **Code** (TypeScript, beside `js`);
  MPEG transport streams are the rare real-world case. Videos documents 11
  extensions; the unique total is **89**.
- **Text conversion** uses `CP_UTF8` with `MB_ERR_INVALID_CHARS` /
  `WC_ERR_INVALID_CHARS` — invalid input is rejected with an explicit error,
  never repaired, never best-fit translated, and the ANSI code page is never
  consulted. Explicit lengths throughout; embedded NUL bytes survive;
  conversion is byte-exact round-trip, never normalizing. Documented Win32
  nuance: with explicit (non `-1`) source lengths, the size query and the
  conversion both return exactly the converted count — no terminator is
  appended or counted.
