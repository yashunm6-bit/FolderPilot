# FolderPilot — Safety Invariants

> **STATUS: PLANNED — NOT YET IMPLEMENTED.**
>
> FolderPilot 0.1.0 is a Phase 1 scaffold. It currently has **no** scanning,
> classification, planning, or movement capability, and therefore currently
> provides **no** file protection or file organization of any kind.
>
> This document records the architectural safety requirements ratified in
> Engineering Plan v2.5. Every invariant below is marked
> **PLANNED — NOT YET IMPLEMENTED**; the guarantee wording must never claim
> more than the implemented mechanism provides. Each invariant is enforced by
> dedicated tests as its phase lands.

---

## 1. Dry-run by default
**PLANNED — NOT YET IMPLEMENTED**
Running the tool without `--apply` builds and displays an organization plan
and performs no filesystem mutation whatsoever. Verified by a snapshot test
that asserts directory state is byte-identical after a dry-run, including
that no category directories are created.

## 2. Explicit authorization before moving files
**PLANNED — NOT YET IMPLEMENTED**
Files move only when the user explicitly passes `--apply`. The executor is
unreachable without it, and the plan executed is exactly the immutable plan
the executor was given — no destination names are invented at execution time.

## 3. No file deletion in the product
**PLANNED — NOT YET IMPLEMENTED**
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
**PLANNED — NOT YET IMPLEMENTED**
Enumeration covers only the immediate children of the validated root.
Subdirectories are never entered, moved, or modified.

## 6. Reparse-point protection
**PLANNED — NOT YET IMPLEMENTED**
Leaf reparse points (symlinks, junctions, OneDrive/cloud placeholders — any
tag) are detected via `FileAttributeTagInfo` on handles opened with
`FILE_OPEN_REPARSE_POINT`, never via `is_symlink`-style checks, and are
skipped/rejected with a reported reason. Ancestor reparse points follow the
approved resolve-and-allow policy: the single path-based bootstrap open
resolves them, and all safety checks apply to the resolved location.

## 7. Hidden/system file protection
**PLANNED — NOT YET IMPLEMENTED**
Entries with `FILE_ATTRIBUTE_HIDDEN` or `FILE_ATTRIBUTE_SYSTEM` are skipped
with a reported reason. The selected target directory and existing category
directories are subject to the same gate: a reparse-point, hidden, system, or
non-directory category is reported unsafe and receives zero planned moves.

## 8. Component-aware path validation
**PLANNED — NOT YET IMPLEMENTED**
PathGuard validates the target using canonical, component-wise comparisons
with depth-policy denylist matching and Windows case-insensitivity — never
naive string prefixes. Drive roots, UNC paths, and denylisted system
locations are refused. The denylist is defense in depth, not a security
boundary.

## 9. Root-relative filesystem operations
**PLANNED — NOT YET IMPLEMENTED**
Exactly one path-based bootstrap open establishes the validated root handle
(reparse inspection, attributes, identity, resolved path all through that
handle). Every subsequent source open, category open, and directory creation
is performed with `NtCreateFile` relative to the retained root handle. The
root object is identity-checked against the plan; no absolute-path opens
exist in the executor.

## 10. Source and destination handle protection
**PLANNED — NOT YET IMPLEMENTED**
Retained root, destination, and source handles are opened **without**
`FILE_SHARE_DELETE`: pre-existing DELETE-capable handles cause a safe
acquisition failure, and once acquired, no normal user-mode process can
rename or delete those objects during the execution window. Source identity
is verified before each move; a replaced source is skipped, leaving both the
original location's contents and the replacement untouched.

## 11. Isolated temporary directories for tests
**PLANNED — NOT YET IMPLEMENTED** (test-infra rule, effective for all future
test phases)
All tests operate exclusively within isolated temporary directories created
by the test-fixture helper. Tests never target the real Downloads folder or
any user data. Canary directories assert that nothing is created or moved
outside the validated root.

## 12. Fail-closed behavior
**PLANNED — NOT YET IMPLEMENTED**
Only confirmed absence permits creating a category directory. Access denied,
invalid paths, unreadable attributes, unverifiable volume identity, and all
other unresolved states fail closed: the affected move or category is skipped
with a specific reported reason, sources and destinations are left untouched,
and independent work continues. Exit codes: 0 success/dry-run, 1 usage,
2 safety refusal, 3 completed with failures, 4 aborted.

---

**Claim discipline:** documentation, report output, and code comments must
never claim stronger protection than the implemented mechanism provides.
This document is itself subject to that rule — wording updates ride along
with the phase that implements the mechanism.
