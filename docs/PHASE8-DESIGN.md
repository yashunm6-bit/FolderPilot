# FolderPilot — Phase 8 Design: Recursive Organization and Retry [RATIFIED 2026-11-19, delegated authority]

Status: design record for the Phase 8 features, self-ratified under the
owner's blanket completion authorization (independent review applied before
implementation; the immutable Phase 7 contract — journal format, storage
layout, safety invariants — is unchanged; Phase 8 features are additive and
every mutation remains journaled through the Phase 7 mechanism).

## 1. Recursive organization

### 1.1 CLI and semantics

`folderpilot <root> --recursive [--apply]` — the dry-run default applies
(preview shows the full recursive plan; `--apply` executes it journaled).
`--recursive` may combine with `--undo`/`--retry`? No: recursive applies to
forward organization only in this phase (undo and retry operate on journals
regardless of how the forward run was produced).

- Deterministic depth-first traversal from the retained root handle.
- Files from ALL depths are planned into the ROOT's category destinations
  (the destination layout is unchanged; nested source directories are never
  moved, created, or deleted).
- Whole-plan collision resolution: candidates from all depths reserve
  destination names through the existing ordinal-case-insensitive resolver
  (planning order = traversal order; deterministic).
- The seven root category destination directories are NEVER traversed as
  source directories (they are destinations; organizing out of them is not a
  ratified behavior).

### 1.2 Traversal safety

- **Reparse points**: any directory carrying a reparse attribute is skipped
  with a structured reason (never traversed; consistent with §3/§6).
- **Hidden/system directories**: skipped with a structured reason.
- **Visited-directory tracking (defense-in-depth)**: every traversed
  directory's volume+file-ID identity is recorded; a directory identity
  encountered twice is skipped as a duplicate (a reparse-free cycle is
  structurally impossible on NTFS since directories are never moved, but the
  tracking is mandated anyway and catches exotic file-system behavior).
- **Containment**: NOT claimed from handle-relative paths alone. Every
  traversal step opens the child through the PARENT's retained handle
  (NtCreateFile with RootDirectory = the parent directory handle) and
  revalidates the opened directory's identity against the identity recorded
  when the parent was enumerated — a concurrent rename/move of a traversed
  directory makes the child open fail (the parent handle's view no longer
  resolves the name), which fails the traversal fail-closed. The traversal
  never follows paths from user input, never re-opens by absolute path, and
  never traverses outside the retained root's subtree by construction; the
  identity-visited set and the parent-handle anchoring together are the
  containment evidence (tested: concurrent rename, replacement, parent-chain
  changes, reparse substitution).
- **Directory-handle lifetime**: each traversed directory is opened with the
  ratified share policy (share R|W, no FILE_SHARE_DELETE) for the duration
  of its own enumeration only; child enumeration completes before the parent
  descends further, so no long-lived locks are held across the whole walk.
- **Access-denied subdirectories**: a subdirectory whose enumeration fails
  (any reason) FAILS THE WHOLE SCAN (fail closed; an incomplete plan is
  never presented as complete). The failure names the offending directory.
- **Files vanishing during planning**: the existing per-entry scan behavior
  (skipped with a reason, per §3/§11 escape-list semantics) — a vanished
  file is not planned; the plan is still complete for what it planned
  (vanishing is an enumerated-entry-level race, not an enumeration failure).
- **Depth limit**: 64 levels; exceeding refuses the whole scan (structured
  reason).
- **Entry limit**: 50,000 planned files + skipped entries in total;
  exceeding refuses the whole scan (memory bound).
- **Long paths**: relative traversal through handles is not subject to the
  MAX_PATH path-length limit; the existing per-name (765-byte journal cap)
  and 255-UTF-16-unit component limits apply and refuse over-long names.
- **Determinism**: entries are ordered per directory by the existing
  ordinal-CI + byte-tiebreak comparator; depth-first order is deterministic.

### 1.3 Journal and undo integration

- Every recursive move is journaled through the unchanged Phase 7.2 session
  (INTENT/RESULT with the file's RELATIVE source path in `source_name`).
- `source_name` cap: the ratified 765-byte limit applies to the relative
  path; the planner refuses files whose relative path exceeds it (fail
  closed, structured reason) — documented, never truncated.
- Undo: reverse renames target `root\<relative source path>` — the recorded
  intermediate directories must still exist at undo time; if a directory was
  removed after organizing, the reverse rename fails and is reported (undo
  never creates directories — restoring locations never invents new ones).
  FileRenameInformation supports multi-component destinations when the
  intermediates exist (W32-DOC).

### 1.4 Retry taxonomy and semantics

Retry is an explicit user-invoked operation (`--retry [--apply]`) — never
automatic. Retryable classification from the NEWEST journal's durable
RESULT outcomes, refined per the authorization:

| Outcome | Class | Retry behavior |
|---|---|---|
| `Moved` | completed | never retried |
| `Locked` | transient per-file | retried only after a fresh preflight re-opens the source and confirms the file is now openable with a matching fingerprint |
| `AccessDenied` | recoverable access denial | retried only after a fresh preflight opens the source with the ratified access (proving the denial has lifted) |
| `Conflict`, `DestinationUnsafe`, `DestinationChanged`, `CrossVolume`, `DestinationInvalid`, `SourceReplaced`, `SourceUnsafe` | permanent destination/source states | never retried (re-plan + fresh `--apply` is the user remedy) |
| `SourceMissing`, `SourceInvalid` | identity/state failures | never retried |
| `Failed` | generic catch-all | NEVER retried without a ratified per-cause mapping (none exists yet) |

Whole-root lock contention is not a move outcome (the lock is acquired
before any move) — a `--retry --apply` invocation that cannot acquire the
lock refuses entirely (exit 2). Ambiguous/blocked/corrupt journal states
refuse the retry entirely (exit 2, flag-immune). Every retry is a fresh
fully-journaled run with a NEW sequence identity; the original journal is
never rewritten. Retry limits: one retry pass per invocation (no internal
loops); repeated invocations are the user's control. Preview: `--retry`
without `--apply` lists the retryable candidates and their fresh-preflight
results (read-only). Exit codes per §2 (blocked → 2; nothing retryable → 0
with a disclosure; partial → 3).

## 2. Implementation map

- `platform/native.{hpp,cpp}`: `open_source_at(root, relative_path)` — the
  ratified source-open policy over a component-validated relative path
  (each component single + safe; no `..`; no drive/UNC/ADS).
- `scanner.{hpp,cpp}`: `ScanOptions::recursive`; `scan_directory_recursive`
  (DFS, identity-visited set, depth/entry caps, category-exclusion, fail-
  closed enumeration); `SkipReason` gains `DepthLimitExceeded`,
  `EntryLimitExceeded`, `DirectoryIdentityDuplicate`.
- `planner.cpp`: recursive entries carry the relative path in `source_name`
  and the basename for classification/destination; relative paths exceeding
  the 765-byte journal cap refuse the plan.
- `executor.cpp`: journaled recursive execution — sources opened via
  `open_source_at`; the INTENT `source_name` is the relative path.
- `undo reverse renames`: destination = root + relative source path (the
  recorded intermediates must exist; otherwise reported failed — never
  auto-created).
- `app/main.cpp`: `--recursive` (forward only), `--retry` (preview/apply).
- Tests: recursive nesting/empty/hidden/system/reparse-at-depth/cycle-
  tracking/nested-collisions/containment/concurrent-rename/depth-and-entry
  limits/determinism/journaled+undo; retry: locked-then-succeeds,
  moved-never, permanent-never, fingerprint-mismatch-skipped, fresh journal
  per retry.

## 3. Security analysis

Recursive traversal widens the read surface (all depths) but adds NO new
mutation surface: destinations remain the root's categories; directories are
never moved/deleted/created (except the ratified category creation); every
file move is journaled and revalidated identically to the non-recursive path.
The traversal's containment evidence = parent-handle anchoring + per-step
identity revalidation + the identity-visited set (tested against concurrent
mutation). Enumeration failures fail closed (no incomplete plans). The retry
path never moves a file whose current fingerprint does not match the journal
record.
