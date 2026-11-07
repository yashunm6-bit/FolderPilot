# FolderPilot — Safety Invariants

> **STATUS: PARTIALLY IMPLEMENTED — read-only planning and dry-run preview (Phase 4).**
>
> FolderPilot 0.1.0 can validate a target, scan it, inspect destination
> categories, resolve collisions, and print a read-only dry-run report
> (Phases 3–4). It still cannot move, create, or delete anything: `--apply`
> is explicitly rejected with "File movement is not available in this build."
>
> This document records the architectural safety requirements ratified in
> Engineering Plan v2.5. Each invariant below carries its own status marker;
> the guarantee wording must never claim more than the implemented mechanism
> provides. Each invariant is enforced by dedicated tests as its phase lands.

---

## 1. Dry-run by default
**IMPLEMENTED (Phase 4) for the preview; the mutation path itself arrives in Phase 5**
Running the tool without `--apply` builds and displays an organization plan
and performs no filesystem mutation whatsoever. Verified by a snapshot test
that asserts fixture state is unchanged after a complete CLI dry-run,
including that missing category directories are not created. There is
currently no code path that mutates user files at all.

## 2. Explicit authorization before moving files
**PLANNED — NOT YET IMPLEMENTED (movement arrives in Phase 5); the `--apply` rejection is IMPLEMENTED (Phase 4)**
Passing `--apply` today returns "File movement is not available in this
build. No changes were made." with a non-zero exit code — it is never
silently ignored. When the executor lands, files will move only through it,
consuming exactly the immutable plan it was given — no destination names
invented at execution time.

## 3. No file deletion in the product
**HOLDS BY CONSTRUCTION (Phases 1–3); re-verified each phase**
Product code contains no delete/remove operations. Files are only ever moved
within the selected root. `remove_all` exists exclusively in test-fixture
cleanup. This is a review-enforced structural invariant.

## 4. No overwriting existing destinations
**PLANNED — NOT YET IMPLEMENTED**
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
**IMPLEMENTED for the bootstrap, enumeration, and source opens (Phase 3); category creation and movement PLANNED (Phases 4–5)**
Exactly one path-based bootstrap open establishes the validated root handle
(reparse inspection, attributes, identity, resolved path all through that
handle). Every subsequent source open is performed with `NtCreateFile`
relative to the retained root handle. Root identity and resolved path are
captured at open time; identity re-verification against the plan arrives
with the planner (Phase 4).

## 10. Source and destination handle protection
**IMPLEMENTED for the open policy and identity capture (Phase 3); execution-window protection PLANNED (Phase 5)**
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
**IMPLEMENTED for validation, scanning, and planning (Phases 3–4); move-time behavior PLANNED (Phase 5)**
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
