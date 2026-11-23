# FolderPilot — Safety Invariants

> **STATUS: IMPLEMENTED through Phase 8 — planning, dry-run, safe executor, journaled undo, recursive organization, and retry.**
>
> FolderPilot 0.1.0 can validate a target, scan it, inspect destination
> categories, resolve collisions, print a read-only dry-run report, and —
> with explicit `--apply` — revalidate everything and move files through
> validated handles with OS-enforced no-overwrite semantics (Phase 5).
> Phase 7 adds journaled execution and undo (`--undo` preview /
> `--undo --apply` restore); Phase 8 adds opt-in recursive organization
> (`--recursive`) and retry (`--retry [--apply]`).
> OS NOTE (disclosed): the Win32 class-3 form of the RootDirectory-relative
> rename (`SetFileInformationByHandle(FileRenameInfo)`) fails with
> ERROR_INVALID_PARAMETER (87) on OS build 26300 — a build-specific
> observation, not a universal Windows guarantee. Movement therefore uses
> the native `NtSetInformationFile` (`FileRenameInformation`, class 10)
> anchored rename, which successfully performed the required movement on
> this build. The complete Debug and Release suites pass locally: 279/279
> cases and 14,893/14,893 assertions per configuration, with 0 failed,
> 0 skipped, and 0 warnings. Hosted CI has verified Phases 1–7.1c (see
> README); there is no hosted Phase 8 claim yet.
> There is still no deletion and no overwrite. Undo restores locations only —
> never file contents.
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
including that missing category directories are not created. Without
`--apply`, no code path mutates user files at all.

## 2. Explicit authorization before moving files
**IMPLEMENTED (Phase 5)**
Files move only when the user explicitly passes `--apply`. The executor
consumes exactly the immutable plan it was given — it never invents
destination names, never re-plans, never retries internally (retry is an
explicit, separate `--retry` invocation, §20). Without `--apply` the
default dry-run prints the plan and changes nothing.

## 3. No file deletion in the product
**HOLDS BY CONSTRUCTION (Phases 1–3); re-verified each phase**
Product code contains no delete/remove operations. Files are only ever moved
within the selected root. `remove_all` exists exclusively in test-fixture
cleanup. This is a review-enforced structural invariant.

## 4. No overwriting existing destinations
**IMPLEMENTED (Phase 5.1 native anchored rename, see banner)**
Movement issues the native `NtSetInformationFile` (`FileRenameInformation`,
class 10) anchored rename with `ReplaceIfExists = FALSE` — the operating
system rejects an existing destination. The older Win32 class-3 form
(`SetFileInformationByHandle(FileRenameInfo)`) fails with
ERROR_INVALID_PARAMETER on OS build 26300 — a build-specific observation,
not a universal Windows guarantee — which is why the native class-10 form is
used. There is no path-based rename fallback.
A conflicting move fails as `Conflict`: the source remains untouched and the
destination content remains unchanged; other independent moves continue.

## 5. No recursive scanning by default (MVP default)
**IMPLEMENTED (Phase 3; Phase 8 adds opt-in `--recursive`)**
By default enumeration covers only the immediate children of the validated
root; without `--recursive`, subdirectories are never entered, moved, or
modified. Phase 8 adds opt-in `--recursive` fail-closed depth-first traversal
(§19): subdirectories are entered only under the structural and
reparse/hidden/system gates, and any enumeration failure fails the whole scan
closed.

## 6. Reparse-point protection
**IMPLEMENTED (Phases 3–5: leaf and target gates in validation/scanning, destination-category gates in planning/execution)**
Leaf reparse points (symlinks, junctions, OneDrive/cloud placeholders — any
tag) are detected via `FileAttributeTagInfo` on handles opened with
`FILE_OPEN_REPARSE_POINT`, never via `is_symlink`-style checks, and are
skipped/rejected with a reported reason. Ancestor reparse points follow the
approved resolve-and-allow policy: the single path-based bootstrap open
resolves them, and all safety checks apply to the resolved location.

## 7. Hidden/system file protection
**IMPLEMENTED (Phases 3–5: entries and target directory in validation/scanning, category-directory gates in planning/execution)**
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
**IMPLEMENTED (Phases 3–5): bootstrap, enumeration, source opens, category revalidation/creation, and movement are all root-relative; NOTE the Win32 class-3 RENAME form is blocked on OS build 26300 (banner) — the native class-10 anchored rename performs the movement root-relative**
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
All filesystem tests operate exclusively inside the synthetic fixture
sandboxes `build/phase3-test-sandbox/` (read-only phase fixtures) and
`build/phase5-test-sandbox/` (movement fixtures), created and cleaned by the
test-fixture helper after canonical-path verification. Tests never target
the real Downloads folder, any user data, or `%TEMP%`.

## 12. Fail-closed behavior
**IMPLEMENTED (Phases 3–5; extended through Phase 8)**
Validation, scanning, planning, and per-move execution all fail closed: a
root identity mismatch aborts the batch before any mutation; a category
replacement is never adopted; a moved-onto destination is rejected by the
OS; one failing move never aborts independent moves.
Invalid paths, missing targets, unreadable attributes, unverifiable volume
identity, uninspectable categories, and all other unresolved states fail
closed: the affected entry or category is skipped with a structured reason,
an invalid plan is rejected whole, and a failed scan is never an apparently
successful empty result. One failing entry or category never aborts
independent work.
Exit codes (per `app/main.cpp`): 0 dry-run report printed / execution
completed with all moves done · 1 usage error · 2 safety rejection (target
refused, root changed, invalid plan) · 3 operational failure — journal
storage I/O, scan or planning failure (including a fail-closed recursive
scan), or execution completed with failures.

---

## 13. The journal is data, never trusted without revalidation
**IMPLEMENTED (Phase 7.2/7.3)**
Every `--apply` run is journaled (write-ahead: RUN-START before the first
move, INTENT flushed and anchored before its rename, RESULT flushed and
anchored after it, RUN-END as the unanchored terminal). Journal records are
evidence, not instructions: undo candidates are re-derived and revalidated
against the live filesystem before every reverse move — the journal is never
trusted without revalidation, and integrity hashes are not tamper protection.

## 14. Undo preview is the default; undo execution requires explicit authorization
**IMPLEMENTED (Phase 7.3/7.4)**
`--undo` is a read-only preview (no lock, no journal writes, no renames —
structurally incapable of mutation). Undo execution requires the explicit
`--undo --apply` combination; without it the undo executor refuses before
touching anything.

## 15. No overwrite in either direction
**IMPLEMENTED (Phases 5/7.3)**
Forward moves use `ReplaceIfExists = FALSE` (the OS rejects an existing
destination); undo reverse renames use the same no-replace mechanism against
the original source slot — a refilled slot is an OS-enforced Conflict
(R16: an unsafe repeated reversal is structurally impossible).

## 16. Write-ahead durability: INTENT before mutation, RESULT after
**IMPLEMENTED (Phase 7.2)**
An INTENT is written and flushed (and its anchor record written and flushed)
before the corresponding rename is permitted; the RESULT is written and
flushed (and anchored) after the mutation. A durable-record failure stops the
run per R1 — later moves are reported as not attempted, never as success.

## 17. Journals, anchors and segments live outside the organized tree
**IMPLEMENTED (Phase 7.1c)**
Storage lives under `%LOCALAPPDATA%\FolderPilot\journals\<root-key>\`
(handle-anchored, reparse-safe, ACL-validated) — never inside the organized
tree, never scanned by FolderPilot, never deleted or rewritten (append-only).

## 18. Coordinated journal/anchor rollback is accepted as undetectable (R25)
**DISCLOSED (Phase 7, accepted limitation)**
A coordinated rollback of the journal and anchor to a mutually consistent
older prefix is undetectable; resurrected candidates fail safe on revalidation
and the limitation is disclosed rather than hidden (§13 confounders, §15 R25).

## 19. Recursive traversal never escapes or duplicates (Phase 8)
**IMPLEMENTED (Phase 8)**
`--recursive` traverses only genuine non-reparse, non-hidden, non-system
directories, reached step-by-step through parent handles from the retained
root (structural containment, never path strings). Reparse points at any
depth are skipped without being opened — a junction can neither move a file
outside the root nor cause duplicate traversal. Category destinations are
never traversed as sources. Depth (64) and entry (50,000) caps and
enumeration failures fail the whole scan closed; no incomplete plan is ever
executed.

## 20. A retry never moves a file whose fingerprint changed (Phase 8)
**IMPLEMENTED (Phase 8)**
`--retry` executes only Locked/AccessDenied outcomes whose fresh preflight
re-opens the source at its recorded location AND matches the recorded
fingerprint (identity digest AND creation time). Everything else — moved,
permanent, identity, or generic failures — is never retried. A blocked or
corrupt newest journal refuses the retry entirely. `--retry --apply` builds
its candidate set from a full recursive scan of the root and fails closed if
any subdirectory cannot be enumerated (PHASE8-DESIGN §1.4). Every retry apply
is a fresh fully-journaled run; the original journal is never rewritten.

---

**Claim discipline:** documentation, report output, and code comments must
never claim stronger protection than the implemented mechanism provides.
This document is itself subject to that rule — wording updates ride along
with the phase that implements the mechanism.
