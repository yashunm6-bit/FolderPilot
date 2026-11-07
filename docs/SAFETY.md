# FolderPilot — Safety Invariants

> **STATUS: PARTIALLY IMPLEMENTED — planning, dry-run preview and the safe executor (Phases 3–5).**
>
> FolderPilot 0.1.0 can validate a target, scan it, inspect destination
> categories, resolve collisions, print a read-only dry-run report, and —
> with explicit `--apply` — revalidate everything and move files through
> validated handles with OS-enforced no-overwrite semantics (Phase 5).
> KNOWN BLOCKER (disclosed): on OS build 26300 the RootDirectory-relative
> rename form fails with ERROR_INVALID_PARAMETER, so movement currently
> fails closed (sources untouched) on this build; no fallback was added.
> There is still no deletion, no overwrite, no undo, and no recursive
> organization.
>
> This document records the architectural safety requirements ratified in
> Engineering Plan v2.5. Each invariant below carries its own status marker;
> the guarantee wording must never claim more than the implemented mechanism
> provides. Each invariant is enforced by dedicated tests as its phase lands.

---

## 1. Dry-run by default
**IMPLEMENTED (Phases 4–5)**
Running the tool without `--apply` builds and displays an organization plan
and performs no filesystem mutation whatsoever. Verified by a snapshot test
that asserts fixture state is unchanged after a complete CLI dry-run,
including that missing category directories are not created. There is
currently no code path that mutates user files at all.

## 2. Explicit authorization before moving files
**IMPLEMENTED (Phase 5)**
Files move only when the user explicitly passes `--apply`. The executor
consumes exactly the immutable plan it was given — it never invents
destination names, never re-plans, never retries. Without `--apply` the
default dry-run prints the plan and changes nothing.

## 3. No file deletion in the product
**HOLDS BY CONSTRUCTION (Phases 1–3); re-verified each phase**
Product code contains no delete/remove operations. Files are only ever moved
within the selected root. `remove_all` exists exclusively in test-fixture
cleanup. This is a review-enforced structural invariant.

## 4. No overwriting existing destinations
**IMPLEMENTED (Phase 5, mechanism); blocked on OS build 26300 (see banner)**
Movement uses `SetFileInformationByHandle(FileRenameInfo)` with
`ReplaceIfExists = FALSE` — the operating system rejects an existing
destination (`ERROR_ALREADY_EXISTS`). There is no path-based rename fallback.
A conflicting move fails as `Conflict`: the source remains untouched and the
destination content remains unchanged; other independent moves continue.

## 5. No recursive scanning in the MVP
**IMPLEMENTED (Phase 3)**
Enumeration covers only the immediate children of the validated root.
Subdirectories are never entered, moved, or modified.

## 6. Reparse-point protection
**IMPLEMENTED for validation and scanning (Phase 3); destination-side gates PLANNED (Phase 4+)**
Leaf reparse points (symlinks, junctions, OneDrive/cloud placeholders — any
tag) are detected via `FileAttributeTagInfo` on handles opened with
`FILE_OPEN_REPARSE_POINT`, never via `is_symlink`-style checks, and are
skipped/rejected with a reported reason. Ancestor reparse points follow the
approved resolve-and-allow policy: the single path-based bootstrap open
resolves them, and all safety checks apply to the resolved location.

## 7. Hidden/system file protection
**IMPLEMENTED for entries and the target directory (Phase 3); category-directory gates PLANNED (Phase 4)**
Entries with `FILE_ATTRIBUTE_HIDDEN` or `FILE_ATTRIBUTE_SYSTEM` are skipped
with a reported reason. The selected target directory is subject to the same
gate and is rejected when hidden, system, or a reparse point.

## 8. Component-aware path validation
**IMPLEMENTED (Phase 3)**
PathGuard validates the target using canonical, component-wise comparisons
with depth-policy denylist matching and Windows case-insensitivity — never
naive string prefixes. Drive roots, UNC paths, and denylisted system
locations are refused. The denylist is defense in depth, not a security
boundary.

## 9. Root-relative filesystem operations
**IMPLEMENTED (Phases 3–5): bootstrap, enumeration, source opens, category revalidation/creation, and movement are all root-relative; NOTE the RootDirectory-relative RENAME form is blocked on OS build 26300 (banner)**
Exactly one path-based bootstrap open establishes the validated root handle
(reparse inspection, attributes, identity, resolved path all through that
handle). Every subsequent source open is performed with `NtCreateFile`
relative to the retained root handle. Root identity and resolved path are
captured at open time; identity re-verification against the plan arrives
with the planner (Phase 4).

## 10. Source and destination handle protection
**IMPLEMENTED (Phases 3 and 5): root/source/destination handles omit FILE_SHARE_DELETE, are identity-checked, and are retained through each movement**
Retained root and source handles are opened **without** `FILE_SHARE_DELETE`:
pre-existing DELETE-capable handles cause a safe acquisition failure, and
while a handle is held, no normal user-mode process can rename or delete the
object. Source identity is captured from the actual opened handle; its
re-verification before each move arrives with the executor (Phase 5).

## 11. Isolated temporary directories for tests
**IMPLEMENTED (Phase 3)**
All filesystem tests operate exclusively inside
`build/phase3-test-sandbox/`, created and cleaned by the test-fixture helper
after canonical-path verification. Tests never target the real Downloads
folder, any user data, or `%TEMP%`.

## 12. Fail-closed behavior
**IMPLEMENTED (Phases 3–5)**
Validation, scanning, planning, and per-move execution all fail closed: a
root identity mismatch aborts the batch before any mutation; a category
replacement is never adopted; a moved-onto destination is rejected by the
OS; one failing move never aborts independent moves. Exit codes: 0 dry-run
report or fully successful execution, 1 usage, 2 target refused or batch
aborted, 3 scan/plan failure or execution with partial failures.
Invalid paths, missing targets, unreadable attributes, unverifiable volume
identity, uninspectable categories, and all other unresolved states fail
closed: the affected entry or category is skipped with a structured reason,
an invalid plan is rejected whole, and a failed scan is never an apparently
successful empty result. One failing entry or category never aborts
independent work. Exit codes: 0 dry-run report, 1 usage/unsupported,
2 target rejected, 3 scan or planning failure.

---

**Claim discipline:** documentation, report output, and code comments must
never claim stronger protection than the implemented mechanism provides.
This document is itself subject to that rule — wording updates ride along
with the phase that implements the mechanism.
