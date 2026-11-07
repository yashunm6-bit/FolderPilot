# FolderPilot — Architecture

> **Implementation status: Phase 5 (safe executor) complete.**
> This document records the architecture approved in Engineering Plan v2.5
> (plus the ratified directory-enumeration mechanism). Only items marked
> **IMPLEMENTED** exist in code today. Everything else is **PLANNED — NOT YET
> IMPLEMENTED**. FolderPilot can validate a target, scan it, classify the
> files, plan the organization, print a read-only dry-run report, and — with
> the explicit `--apply` flag — execute the planned movements through
> validated handles with OS-enforced no-overwrite semantics. Undo, retries,
> and recursive organization remain out of scope.

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
| PathGuard: target validation, attribute gates, component-aware denylist | **IMPLEMENTED** (Phase 3) |
| Root bootstrap: single path-based `NtCreateFile` open, RAII handle, resolved path, identity, volume GUID | **IMPLEMENTED** (Phase 3) |
| Directory scanner: handle-anchored enumeration, non-recursive, structured skip reasons | **IMPLEMENTED** (Phase 3) |
| Source identity capture from opened handles (`FileIdInfo` + separate legacy fallback) | **IMPLEMENTED** (Phase 3) |
| Platform layer: native file handling (`NtCreateFile`, runtime-resolved ntdll functions) | **IMPLEMENTED** (Phase 3, bootstrap + source opens) |
| Destination category inspection (root-relative, read-only, snapshot of existing names) | **IMPLEMENTED** (Phase 4) |
| Collision resolution during planning (reserved natural names, bounded suffixes) | **IMPLEMENTED** (Phase 4) |
| Immutable `OrganizationPlan` with validated invariants | **IMPLEMENTED** (Phase 4) |
| Dry-run Reporter (render-only, no filesystem access) | **IMPLEMENTED** (Phase 4) |
| Read-only CLI (`folderpilot <dir>`, `--help`, `--version`; `--apply` explicitly rejected) | **IMPLEMENTED** (Phase 4) |
| Root revalidation (identity/path/volume) before any mutation | **IMPLEMENTED** (Phase 5) |
| Category revalidation + root-relative category creation (`FILE_CREATE`) | **IMPLEMENTED** (Phase 5) |
| Source revalidation + identity compare + volume guard per move | **IMPLEMENTED** (Phase 5) |
| Handle-based no-overwrite movement (`FileRenameInfo`, ReplaceIfExists=FALSE) | **IMPLEMENTED** (mechanism; blocked on OS build 26300, see §11) |
| ExecutionResult + execution report (partial-failure semantics) | **IMPLEMENTED** (Phase 5) |
| Undo, retries, recursive organization | PLANNED — NOT YET IMPLEMENTED (Phase 6+) |
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
- Phase 3 suites: `test_native` (pure parser/comparator/identity),
  `test_path_guard` (pure denylist + sandboxed bootstrap),
  `test_scanner` (sandboxed integration) with fixture helpers in
  `phase3_fixture.*`.
- Phase 4 suites: `test_collision` (pure resolver), `test_planner`
  (pure invariants + sandboxed planning), `test_reporter` (render-only),
  `test_cli` (spawns the real executable; read-only dry-run immutability,
  `--apply` rejection, exit codes).
- Phase 3 filesystem fixtures live exclusively under
  `build/phase3-test-sandbox/` (never `%TEMP%`, never user folders); cleanup
  removes only the verified fixture directory. Capability-gated tests
  (junctions) report skips explicitly.
- All future integration tests operate exclusively in isolated temporary
  directories; tests never target real user data. `remove_all` appears only
  in test-fixture cleanup, never in product code.

## 7. Phase map

| Phase | Scope | Status |
|---|---|---|
| 1 | Project scaffold and build system | **IMPLEMENTED** |
| 2 | Core types and classification | **IMPLEMENTED** |
| 3 | PathGuard and directory scanner | **IMPLEMENTED** |
| 4 | Planner and dry-run | **IMPLEMENTED** |
| 5 | Safe executor | **IMPLEMENTED** (movement blocked on OS build 26300, see §11) |
| 6 | Hardening, tests and documentation | PLANNED |

## 10. Phase 4 implementation notes (IMPLEMENTED)

- **Workflow**: SCAN → CLASSIFY → INSPECT DESTINATIONS → PLAN → REPORT.
  The CLI runs exactly this pipeline read-only and exits; nothing is ever
  created, moved, or deleted, and `--apply` is rejected with an explicit
  message and a non-zero exit code.
- **Destination category inspection**: each of the seven category folders is
  opened relative to the retained root handle (`NtCreateFile` with
  `RootDirectory`, `FILE_OPEN`, `FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT`),
  minimum access (`FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE`),
  leaf un-followed, handle closed after inspection. Safe categories get a
  full name snapshot (every entry counts as a collision, including hidden,
  system, reparse, and directories); only a confirmed not-found classifies a
  category as Missing (never created in Phase 4); anything else is Unsafe or
  Uninspectable and receives zero planned moves, without aborting safe
  categories.
- **Collision resolution** happens entirely during planning, per category:
  all incoming natural names are reserved FIRST (a generated suffix can
  never steal another incoming file's natural destination), then each
  candidate takes its natural name if free or the first free `name (n)ext`
  candidate, matching ordinally case-insensitively against the destination
  snapshot, all reservations, and earlier allocations. Compound extensions
  (`.tar.gz` …), dotfiles, and extensionless names keep their full shape;
  suffix attempts are bounded (default 100) and exhaustion is reported as
  `CollisionExhausted` — never an unbounded search, never an invented
  alternative directory.
- **The plan is immutable**: private storage, no mutators, read-only
  accessors; invariants (single-component destinations, pairwise-unique
  destinations, category agreement, no unsafe-category targets, no conflicts
  with existing entries) are validated before publication; an invalid plan
  is rejected whole (fail closed) instead of published partially.
- **Reporter** is render-only: it consumes the plan and prints target,
  resolved path, per-category sections marked "existing" or
  "will be created", skip reasons, collision-resolved names, summary counts,
  and the NO FILES WERE MOVED / NO DIRECTORIES WERE CREATED / NO FILES WERE
  DELETED footer. Internal `.`/`..` noise is filtered from the user view.
- **CLI exit codes**: 0 dry-run report · 1 usage error / unsupported feature
  (`--apply`) · 2 target rejected · 3 scan or planning failure.
  Known limitation (documented, deferred): CLI arguments use the ANSI entry
  point, so non-ASCII directory arguments may not round-trip until the wide
  entry point lands in a later phase.

## 9. Phase 3 implementation notes (IMPLEMENTED)

- **Bootstrap**: exactly one path-based open per target, via
  runtime-resolved `NtCreateFile` (no conflicting NT prototypes anywhere);
  the DOS path is classified first (relative / drive-relative / UNC / device
  / ADS / forward-slash / embedded-NUL all rejected), converted with
  `RtlDosPathNameToNtPathName_U`, and the resulting NT form is verified
  (`\??\X:`) before use. Access: `FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
  | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY | SYNCHRONIZE`; share
  `FILE_SHARE_READ | FILE_SHARE_WRITE` (no `FILE_SHARE_DELETE`); options
  `FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT |
  FILE_SYNCHRONOUS_IO_NONALERT`; disposition `FILE_OPEN` (never creates).
- **Verified payload**: attributes via `FileAttributeTagInfo`, resolved
  final path via `GetFinalPathNameByHandleW` (extended-length and UNC
  prefixes recognized explicitly), identity via `FileIdInfo` with the legacy
  `GetFileInformationByHandle` fallback kept as a strictly separate variant,
  volume identity via `GetVolumePathNameW` → normalization (exactly one
  trailing backslash) → `GetVolumeNameForVolumeMountPointW` (local volume
  GUIDs only).
- **Denylist**: component-aware over the resolved path with
  `CompareStringOrdinal` case-insensitive matching; drive roots rejected;
  eight system subtrees rejected; `Users` node + direct children rejected,
  deeper allowed; current-user home root rejected. No string-prefix
  matching anywhere; evaluation is a pure, unit-tested function.
- **Enumeration**: `GetFileInformationByHandleEx` with
  `FileIdBothDirectoryRestartInfo` then `FileIdBothDirectoryInfo` batches on
  the retained root handle; strict parser validates header bounds, 8-byte
  alignment of `NextEntryOffset`, even `FileNameLength`, and name-region
  bounds; the chain ends at `NextEntryOffset == 0` (the Win32 API does not
  report the written byte count). A single entry larger than the buffer
  fails the scan rather than being skipped.
- **Scanner**: non-recursive; `.`/`..`, directories, reparse points
  (any tag), hidden, and system entries are skipped with structured reasons
  before any open; candidates are opened relative to the root handle
  (`DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE`, share R|W, no
  `FILE_SHARE_DELETE`, `FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT`);
  the opened handle is the source of truth for attributes, size, times, and
  identity; every source handle is closed after inspection. A single
  failing entry never aborts the batch; an enumeration failure fails the
  whole scan (never an apparent empty success).
- **Determinism**: results are sorted with ordinal case-insensitive
  comparison and a stable byte-wise tie-break; name-equal entries are never
  deduplicated.
- **Known platform observations** (documented, handled): some filesystem
  surfaces return `.`/`..` through directory enumeration — the scanner
  skips them defensively as `ReservedName`; a directory reparse point
  replacing a candidate file is rejected by the `FILE_NON_DIRECTORY_FILE`
  open (`NotRegularFile`) before any tag check — both outcomes are safe
  rejections of the link.

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

## 11. Phase 5 implementation notes (IMPLEMENTED) and OS finding

- **Executor flow**: plan validation -> fresh PathGuard bootstrap -> root
  identity/path/volume comparison against the plan (any mismatch aborts the
  whole batch with RootChanged BEFORE any mutation) -> per-category
  revalidation (Safe: re-open + identity compare, replacement/deletion is
  DestinationChanged; Missing: root-relative `FILE_CREATE` creation, a
  collision is DestinationChanged, never adopted) -> per-move source
  revalidation (open relative to the retained root, attribute gate, identity
  compare, volume-serial guard) -> handle-based rename ->
  per-move structured outcome.
- **Movement**: `SetFileInformationByHandle(FileRenameInfo)` with
  `ReplaceIfExists = FALSE`, `RootDirectory` = validated category handle,
  relative single-component name, variable-length buffer sized
  `sizeof(FILE_RENAME_INFO) + name_bytes` (see the contract note below).
  No path-based rename exists anywhere; no fallback.
- **OS FINDING (build 26300, disclosed)**: the RootDirectory-relative form
  of `FileRenameInfo` fails with ERROR_INVALID_PARAMETER (87) for every
  handle/flag/size/null convention probed (12+ combinations, evidence in
  the test suite), while absolute-path forms succeed. The approved
  root-relative movement mechanism is therefore BLOCKED on this OS build.
  Per the gate rules no absolute-path fallback was implemented; movement
  tests are capability-gated and report explicit SKIPPED reasons (9 cases).
  The executor fails closed: every move reports Failed, sources and
  destinations stay untouched, directories created before the block remain
  (documented in the execution report).
- **Contract correction found by the probe**: the FILE_RENAME_INFO buffer
  must be at least `sizeof(FILE_RENAME_INFO) + FileNameLength` bytes - a
  plain `offsetof(FileName) + bytes` buffer is rejected with 87 even for
  absolute-path renames. The rename buffer sizing was fixed accordingly.
- **CLI**: `--apply` executes the plan; exit 0 all moved, exit 3 partial
  failures, exit 2 safety abort (RootChanged/invalid plan). Dry-run remains
  the default and remains mutation-free.
