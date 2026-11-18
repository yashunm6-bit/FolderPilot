# FolderPilot — Phase 7 Design: Move Journal & Undo (Ratified)

> **STATUS: Contract RATIFIED (G2, G3, G4, G6, G7, G8, G9, G10 as recommended; G5 deferred
> to 7.4). G1: BCrypt chosen and CLOSED — hosted run `43598561201` (commit `dea0dc6`)
> verified the bcrypt linkage and all four NIST vectors on `windows-latest` (2026-11-14).
> Phase 7.1b (journal records) IMPLEMENTED and hosted-verified at `bc520c2` (run
> `43924678919`: Debug and Release each 200/200 cases, 13,843/13,843 assertions, zero
> failed/skipped); §6.3/§12/§15 reconciled to the verified implementation (2026-11-16).
> Phase 7.1c (storage adapter) IMPLEMENTED and hosted-verified at `e98089f` (run
> `44341177181`: Debug and Release each 225/225 cases, 14,112/14,112 assertions, zero
> failed/skipped; artifact SHA-256 verified byte-level against the downloaded bytes;
> genuine cross-process lock contention and crash-release child-process tests pass
> locally and hosted; ratified Decisions A–E recorded in §16).**
> Base: `main` @ `823dfed`; design ratified in commit `cc671a3`; G1 closure recorded
> 2026-11-14 (record date 2026-11-13). Labels: **[W32-DOC]** documented Windows API behavior · **[SRC]** verified
> repository source/behavior · **[SETTLED]** user decision · **[REC]** recommendation ·
> **[GATE]** open decision · **[ASSUMPTION]** unverified.

---

## 1. Objective and scope

**Phase 7 objective: Move Journal & Undo** — make every `--apply` operation recoverable
through a durable, integrity-checked journal and an explicitly invoked, fully revalidated
reverse mode.

**Included:** write-ahead journal for forward `--apply`; integrity-checked persistent
journal; undo preview by default; explicit `--undo --apply`; full identity-based
revalidation; undo operation journaling; recovery and fail-closed handling; tests and
documentation.

**Excluded (deferred to Phase 8):** recursive file organization; retry handling.
Also excluded: selective/partial undo (whole-last-run only), chained multi-run undo,
moving/organizing directories, config-driven rules, UNC/network roots (already rejected
by PathGuard), external checkpoint subsystems (G10).

**Preserved principles [SETTLED]:** dry-run/preview by default in both modes · explicit
`--apply` before any mutation · never overwrite (`ReplaceIfExists = FALSE` in both
directions) · fail-closed on every ambiguity · non-recursive scanning ·
`plan → validate → execute` flow unchanged · Win32 only inside `platform/` · synthetic
test fixtures only · no automatic journal deletion or rewriting · Smart App Control stays
ON; verification through hosted CI.

## 2. CLI grammar (exact)

```
folderpilot <root>                                     forward dry-run (default; mutation-free)
folderpilot <root> --apply                             forward execute (journal + anchor written)
folderpilot <root> --undo                              undo preview (default; mutation-free)
folderpilot <root> --undo --apply                      undo execute (attempt segment written)
folderpilot <root> --apply --acknowledge-undo-window   forward execute, durably superseding a resolved live undo window
folderpilot --help | --version
```

Mode (`--undo`) and authorization (`--apply`) are **orthogonal** — they are not mutually
exclusive. `--acknowledge-undo-window`: valid **only** with forward `--apply` (with
`--undo` or without `--apply` ⇒ usage error, exit 1); never bypasses blocked/unresolved
chains, torn/ambiguous states, integrity failures, or the mutation lock (exit 2
regardless); never authorizes overwriting; requires the acknowledgement to be journaled,
flushed and anchored before the first mutation; produces the `SUPERSEDED UNDO
OPPORTUNITY` report section before the first mutation. Exit codes: 0 success/preview ·
1 usage · 2 safety refusals · 3 operational failures [G3].

## 3. Storage and locking

Handle-anchored chain from one path-based bootstrap of `%LOCALAPPDATA%`
(`SHGetKnownFolderPath(FOLDERID_LocalAppData)` [W32-DOC]): every subsequent component is
opened/created **relative to the parent handle** (`NtCreateFile`, `RootDirectory` =
parent), reparse-inspected via `FileAttributeTagInfo` on the opened handle; reparse at
any component ⇒ refuse. Hidden/system gates apply only to FolderPilot-created components
(`FolderPilot`, `journals`, `<root-key>`); the legitimately hidden `AppData` ancestor is
accepted [W32-DOC: `AppData` carries `FILE_ATTRIBUTE_HIDDEN`]. `<root-key>` identity is
re-verified every session; ACL **validation** (not construction) of the journals
directory [G4] — operability scope per ratified Decision D, with absent and NULL
DACLs refused outright and empty DACLs denied through AccessCheck per ratified
Decision A (2026-11-17/18). Journal, segment and anchor files are never located inside the organized
tree and are never scanned by FolderPilot.

**Leaf-open protocol — `FILE_OPEN_REPARSE_POINT` is mandatory on every open that can
encounter an existing leaf [W32-DOC: the flag opens the reparse point itself; no target
resolution occurs during the open]:**

| Leaf | Disposition | Options | Access / share | Post-open validation |
|---|---|---|---|---|
| Lock (first) | `FILE_OPEN_IF` + `FILE_OPEN_REPARSE_POINT` | — | `SYNCHRONIZE\|FILE_READ_ATTRIBUTES\|FILE_READ_DATA` / **0** | tag = regular, non-reparse ⇒ hold |
| Lock (later) | `FILE_OPEN` + `FILE_OPEN_REPARSE_POINT` | — | same | same |
| Journal/segment (fresh seq) | `FILE_CREATE` (refuse-on-exists) | — | `GENERIC_WRITE\|FILE_READ_ATTRIBUTES\|SYNCHRONIZE` / `FILE_SHARE_READ` | regular file |
| Journal/segment/anchor (read) | `FILE_OPEN` + `FILE_OPEN_REPARSE_POINT` | — | `FILE_GENERIC_READ` / R\|W\|D | regular, non-reparse |

**Access-mask corrections [SRC, runtime evidence 2026-11-18]:** the lock masks
carry `FILE_READ_DATA` because sharing checks exempt attributes-only access —
without a data bit the holder's share-0 could never conflict and the probe's
`held` outcome (and exclusive re-acquisition refusal) would be unreachable.
The create mask carries `FILE_READ_ATTRIBUTES` because the mandated post-open
validation (`FileAttributeTagInfo`, identity capture) fails `ACCESS_DENIED`
against a write-only handle. The journals-directory ACL validator
AccessChecks with `DACL\|OWNER\|GROUP` information and a Query-level
impersonation duplicate of the process token: `AccessCheck` fails
`ERROR_BAD_IMPERSONATION_LEVEL` against a primary token and
`ERROR_INVALID_SECURITY_DESCR` against a group-less descriptor (a
`SetNamedSecurityInfoW` DACL write leaves the un-requested group out of the
returned descriptor). The aggregate-quota recompute skips the `.`/`..`
prefix records (scanner `ReservedName` parity) and refuses every other
directory entry.

Unexpected type or reparse tag ⇒ structured refusal (`ReparseLeaf`/`NotRegularFile`);
no deletion, no traversal, no fallback.

**Lock protocol:** holder access `SYNCHRONIZE|FILE_READ_ATTRIBUTES`, share 0 ⇒ any second
acquirer fails `SHARING_VIOLATION` [W32-DOC]; probe = attributes-only open, full share
(violation = held · `NAME_NOT_FOUND` = never created · success = free, closed
immediately); held across the complete operation (forward: acquisition → scan → plan →
journal → all moves → close; undo: acquisition → verification → all reverse moves →
close); kernel closes handles at termination ⇒ **crash-release with no stale state**
[W32-DOC]; `lock_id` recorded in RUN-START and re-verified cross-session, mismatch ⇒
refuse; the lock is never deleted and never opened with DELETE access. Journal writers
open with `GENERIC_WRITE|SYNCHRONIZE` + share R (GENERIC_WRITE is required by
`FlushFileBuffers` [W32-DOC]), which is reader-compatible with preview readers
(`FILE_GENERIC_READ`, share R|W|D) [W32-DOC sharing arithmetic]; fallback = explicit busy
refusal for undo-preview; forward dry-run never reads journals.

## 4. Anchor protocol

**Scope:** forward journals **anchored** (RUN-START, INTENT, RESULT); **RUN-END is an
unanchored terminal record**, exempt from anchor ordering; **undo-attempt segments are
unanchored in Phase 7** (suffix truncation there is undetectable and documented — a
segment-anchoring alternative may be separately ratified later [G9]).

**Ordering — per journal record R:** `journal append(R) → journal flush → anchor
append(A_R) → anchor flush → next permitted operation`. An INTENT's rename is permitted
**only after the INTENT is anchored** (journal flush AND anchor flush both confirmed);
the next move only after its RESULT is anchored; RUN-START is anchored before the first
INTENT is permitted.

**Coverage model:** `covered(A)` = the journal prefix proven by A's last **complete**
record (`covered_seq`, `byte_len`, `last_record_hash`); `uncovered(J)` = the remainder.
Post-restart classification is **evidence-only** — a live process may report its own
observed write/flush failures precisely, but that knowledge is ephemeral and never
persisted (R24).

### 4.1 Classification of `uncovered(J)` after reopening

| Uncovered content | Class | Reason |
|---|---|---|
| **Torn INTENT** | **Ambiguous ⇒ BLOCKED** | a previously complete flushed INTENT may have been truncated/corrupted into a torn remnant; the files cannot exclude a previously authorized rename |
| **Complete INTENT** | **Ambiguous ⇒ BLOCKED** — even if the only uncovered record | indistinguishable between "anchor never written (W3/W4: rename never attempted)" and "anchor written then lost/truncated (rename attempted)" — the scenario RUN-START anchored → INTENT anchored → rename succeeds → RESULT unwritten → final anchor record lost |
| **Torn RESULT** (following a covered INTENT) | **Ambiguous ⇒ BLOCKED** | a RESULT is written only after the rename ⇒ the rename was attempted; outcome unknown |
| **Complete RESULT** (following a **fully covered** INTENT) | **Classified by its recorded outcome + durability caveat** | the RESULT's existence proves the rename was attempted and finished; `Moved` ⇒ the candidate exists (post-crash loss of the record re-classifies safely); failed ⇒ source intact, no candidate |
| **RUN-START fully covered, no INTENT anywhere in J** | **Declared-inert** | renames are strictly gated on anchored INTENTs and the append-only journal contains none ⇒ no rename was ever attempted, provable from J alone |
| **RUN-START uncovered (anchor missing/empty), no INTENT in J** | **Declared-inert with anchor-absent disclosure** | same journal-internal proof |
| **RUN-END complete, unanchored** | Terminal: closed with durability caveat | nothing mutation-capable follows |
| **Torn terminal RUN-END** | Terminal rules: unterminated-but-complete, disclosed | bookkeeping only |
| More than one uncovered record, or any impossible transition | **Protocol violation ⇒ BLOCKED** | the protocol stops at the first failure; ≤1 uncovered record is reachable |
| Zero complete records — clean empty, or torn head uniquely identifying RUN-START | **Abandoned-forward-initialization** | no INTENT possible; sequence permanently consumed [G2] |
| Zero complete records — torn head untyped, or typed INTENT/another impossible first record | **BLOCKED** | a torn tail that cannot reveal its type, or identifying an impossible first record, cannot exclude a previously authorized rename (R20) |

**Torn/failed anchor tail:** harmless **only** when `uncovered(J)` is empty; if
`uncovered(J)` is non-empty, a torn or failed anchor tail **may have covered an INTENT
whose anchoring is now unprovable ⇒ BLOCKED**. An anchor suffix is never classified as
harmless debris while the journal may contain an operation whose anchor was lost.
**Missing/empty anchor:** journal has any INTENT ⇒ blocked; journal has **no INTENT** ⇒
declared-inert with anchor-absent disclosure.

**Hash chain and `seq`:** interior integrity and ordering only — **neither detects
suffix truncation**; never presented as such.

**Initialization (exact):** journal `FILE_CREATE` → anchor `FILE_CREATE` (same safe leaf
protocol) → both handles validated + identities captured (`anchor_id` in RUN-START,
`journal_id` in the anchor genesis record) → RUN-START written+flushed → genesis anchor
record covering RUN-START → first INTENT permitted. Crash windows: journal-only ⇒
abandoned-forward-initialization; both files, RUN-START absent ⇒ abandoned-forward-
initialization; RUN-START without its anchor record ⇒ **declared-inert** (no INTENT
possible yet); anchor missing + journal has INTENTs ⇒ blocked. Sequences permanently
consumed in every case; correlation via `root_run_seq` + `anchor_id`/`journal_id`;
mismatch ⇒ refuse.

## 5. Journal schemas and parser invariants

Canonical JSONL, fixed key order, strict UTF-8, LF-terminated; `integrity` = SHA-256
over canonical record bytes excluding `integrity`; `prev` = SHA-256 of the previous
record's canonical form (genesis = SHA-256 over root-key ‖ `root_run_seq` ‖ `run`) —
**interior integrity/ordering only; neither the hash chain nor `seq` detects suffix
truncation.** All size caps apply to the **canonical serialized form** (JSON escaping
included, terminating LF counted) — including an unterminated final record, whose
canonical form counts the LF its writer would have written. `prev` is carried by every
journal and segment record (genesis = seed) and never by anchor records; undo records
carry **no** `move_id` (the reversed move is identified by `undo_of_move`).

**Common (all records):** `v` ≤3 · `run` ≤34 · `dir` ≤9 · `type` ≤13 · `seq` ≤8
(per-file, 0-based, gap-free) · `root_run_seq` ≤20 · `prev` ≤66 · `integrity` ≤66.

**Forward journal:**

| Field | Records | Serialized ceiling |
|---|---|---|
| `planned_move_count` | RUN-START | ≤8 |
| `lock_id`, `root_id`, `anchor_id` | RUN-START | ≤73 each |
| `volume`, `root_path` | RUN-START | ≤60 / ≤4,096 (`\` escaping counted) |
| `undo_window_acknowledged`, `supersedes_root_run_seq` | RUN-START | ≤5 / ≤20 |
| `move_id` | INTENT/RESULT | ≤8 |
| `cat_id`, `file_id` | INTENT/RESULT | ≤73 each |
| `file_ctime` (gating), `file_size`, `file_mtime` (supplementary) | INTENT/RESULT | ≤20 each |
| `source_name`, `dest_name` | INTENT/RESULT | ≤765 each (255 UTF-16 units ⇒ ≤765 B UTF-8; components cannot contain `"`, `\` or <0x20 [W32-DOC] ⇒ no escape expansion) |
| `dest_cat`, `outcome`, `detail` | INTENT/RESULT | ≤11 / ≤20 / ≤514 |
| summary counts | RUN-END | ≤8 each |

**Undo-attempt segment:** common + `fwd_journal_digest` ≤66 (**every record**, constant
per segment) · `undo_attempt_seq` ≤20 (filename ordering) · `journal_id` ≤73,
`attempt_candidate_count` ≤8 (UNDO-START) · `attempt_move_seq` ≤8, `undo_of_move` ≤8 ·
the same identity/evidence/name/outcome fields · summary counts (UNDO-END).

**Anchor:** `v`, `kind:"anchor"`, `root_run_seq`, `file_kind:"journal"`, `journal_id`,
`anchor_seq`, `covered_seq`, `byte_len`, `last_record_hash`, `integrity` — each record
≤450 B.

Parser invariants: unknown `type`/`v` ⇒ invalid; `seq`/`anchor_seq` gaps/rewinds ⇒
invalid; expected-record transitions (§6) enforced; correlation contradictions (duplicate
`move_id`, double-restored, restore-of-unmoved, digest mismatch, acknowledgement
contradictions) ⇒ invalid; caps: record ≤8 KiB, `detail` ≤512 B, `root_path` ≤4,096 B.
Implementation (modern-cpp, **C++20 ceiling** per the project pin): pure canonical
serializer, `std::span`, `enum class`, `[[nodiscard]]`, ok/error result structs (no
`std::expected` — C++23), no iostream in the serializer.

## 6. State machines

**6.1 Forward.** Lock → eligibility (§6.5) → journal `FILE_CREATE` (`root_run_seq` =
max allocated + 1) → anchor `FILE_CREATE` → identity capture → RUN-START(+flush+anchor)
→ per move: INTENT(+flush+anchor) ⇒ **rename permitted** ⇒ RESULT(+flush+anchor) ⇒ next
move permitted → RUN-END(+flush, unanchored terminal). Failure windows per §10; any
window ⇒ stop per R1.

**6.2 Attempt segment (unanchored).** Lock → eligibility → segment `FILE_CREATE`
(`undo_attempt_seq`+1) → UNDO-START(+flush) → per candidate (`attempt_move_seq` order):
UNDO-INTENT(+flush) ⇒ **reverse rename permitted** ⇒ UNDO-RESULT(+flush) ⇒ next
candidate → UNDO-END(+flush, unanchored terminal). Stop rules per §10; segments are
never declared inert after reopening (§10, segment rows).

**6.3 Forward completeness states** (classified against RUN-START's
`planned_move_count`):
- **A — unterminated but complete:** every planned move has a final RESULT; RUN-END
  absent or torn-terminal. Resolved; undo-eligible, disclosed.
- **B — interrupted but unambiguous partial:** a prefix k < N has final RESULTs and **no
  INTENT record exists beyond the prefix** (write-before-rename ordering proves moves
  k…N−1 were never attempted). Resolved; candidates = confirmed Moved in the prefix;
  report "interrupted after k of N planned moves; N−k never attempted" + supersession
  warning.
- **C — unresolved:** any dangling **covered** INTENT/UNDO-INTENT, any torn or complete
  uncovered INTENT, any torn RESULT/UNDO-RESULT beyond coverage, torn-within-coverage,
  truncation, chain break, or correlation anomaly ⇒ **blocked** (permanence; only a
  future designed `--recover` [G7] changes it).
- **Declared-inert:** RUN-START-only, covered or not ⇒ zero INTENT records ⇒ provably no
  mutation (bounded by R25); non-blocking; dormant ack preserved; disclosed. (The
  earlier draft's "RUN-START + torn-INTENT tail" extension is **withdrawn**:
  torn-tail classification is expected-record-aware per §4.1/R20/T-60.)
- **Abandoned-forward-initialization:** zero complete records — a clean empty journal,
  or a torn head that uniquely identifies RUN-START ⇒ non-blocking, sequence
  permanently consumed, byte-identical preservation, disclosed, excluded from
  newest-operation-bearing selection [G2]; an untyped torn head, or one identifying
  INTENT or another impossible first-record type, ⇒ **blocked** (fail-closed, R20).

**6.4 Attempt-level states:** segment zero-complete-records/torn-UNDO-START ⇒
**abandoned-attempt-initialization** — `undo_attempt_seq` permanently consumed, segment
preserved byte-identically, disclosed, a later attempt uses the next sequence when the
chain is otherwise resolved; **distinguished from a dangling covered UNDO-INTENT ⇒
blocked**. Segment complete-unterminated (all candidate records final, UNDO-END absent
or torn-terminal) ⇒ fold decides; disclosed.

## 7. END-record classification (evidence-based)

Reopen, parse, classify — **never by the API's failure return**: complete parsable END
⇒ **closed** (flush-failure ⇒ durability caveat [W32-DOC: written bytes survive process
death; power-loss durability unconfirmed]); END absent with all preceding records
complete ⇒ forward: states A/B per `planned_move_count`; undo: **the fold decides**
(§9.3) — **UNDO-END absence never implies zero candidates**; torn/malformed final record
⇒ per §4/§10. **The journal is never written after its owning operation ends — no repair
passes, ever [G6].**

## 8. Durable acknowledgement protocol

RUN-START carries `undo_window_acknowledged` + `supersedes_root_run_seq` **only** when
forward `--apply` passed a live undo window via `--acknowledge-undo-window`; RUN-START is
**flushed and anchored before the first INTENT is permitted**. Parser invariants:
`acknowledged:true` ⇒ `supersedes_root_run_seq` present and resolvable to a parseable
earlier forward journal; `acknowledged:false` ⇒ field absent; a fresh `acknowledged:false`
journal while a live window exists ⇒ invalid (unreachable except by tampering).
**Activation: the first successfully anchored INTENT.** An unanchored INTENT never
activates supersession (no rename could have occurred). **Ambiguity:** loss of that
first INTENT's anchor ⇒ complete uncovered INTENT ⇒ chain **blocked** — the prior window
is **neither re-declared preserved nor the new run declared dormant**; reports state
"supersession status unknown — root blocked". Crash between RUN-START and the first
INTENT ⇒ dormant ack preserved, run declared-inert, older window preserved. Previews and
reports disclose dormant|active|unknown.

## 9. Undo-candidate fold, eligibility, reconciliation

**9.1 Fold (exact):**
`live_candidates(F) = { m ∈ F.moves : m.result.outcome = Moved } − { m : ∃ attempt
segment s, record r : r.undo_of_move = m.move_id ∧ r.outcome = restored }`
— evaluated by folding the forward journal and **every** attempt segment; requires every
chain file resolved; a final recorded reverse failure does **not** remove the candidate;
**missing/torn UNDO-END never implies zero candidates**; a later attempt with zero live
candidates creates **no segment** ("nothing eligible", exit 0).

**9.2 Attempt eligibility:** newest forward chain resolved ∧ live candidates ≥ 1 ∧
capacity admitted (exact per-attempt check) ∧ root lock acquirable.

**9.3 Reconciliation [corrected table]:** for a chain blocked/ambiguous at the segment
level, read-only reconciliation evaluates each forward `Moved` candidate m
(`source_name` → `dest_cat`/`dest_name`) using the fingerprint (file ID + creation time):

| Observed evidence | Interpretation |
|---|---|
| Matching identity **at the destination**; **original source slot free** | **Still forward-moved ⇒ eligible for undo** |
| Matching identity **at the original source slot**; **absent from the destination** | **Already restored ⇒ removed from live candidates** |
| Both occupied, both absent, identity mismatch, or a replacement present | **Ambiguous ⇒ refuse (per-move)** |

Reconciliation is **not an atomic snapshot**: the expected identity and
source/destination conditions are **revalidated immediately before the reverse rename**
by the executor's normal per-move revalidation. Reconciliation never mutates and never
clears an ambiguous segment. **Refusal conditions:** fingerprint mismatch, both-present,
both-absent, replacement ⇒ per-move refusal + report.

**9.4 Resurrected-candidate safety (R16):** a lost/truncated `restored` record
resurrects a candidate; a later attempt's reverse rename then (a) finds the destination
absent ⇒ `SourceMissing` ⇒ fails closed, or (b) finds an unexpected destination file ⇒
no-overwrite `Conflict` ⇒ fails closed; the source-slot fingerprint check fails for any
replacement ⇒ fail closed. **An unsafe repeated reversal is structurally impossible.**

**9.5 Eligibility and restrictions:**

| Newest chain state | Undo preview | Undo apply | Forward apply |
|---|---|---|---|
| A/B, ≥1 live candidate | allowed | allowed | **refused (exit 2)** unless `--acknowledge-undo-window` (durable+anchored ack §8) |
| A/B, 0 live candidates | allowed ("nothing eligible") | no segment created | allowed, disclosure only |
| C/blocked/ambiguous segment | allowed (read-only evidence; reconciliation §9.3) | reconciliation-provable subset only; unknown ⇒ per-move refusal | **refused** (flag-immune), exit 2 |
| declared-inert / abandoned-init newest | disclosed; older window preserved | targets older operation-bearing run | allowed (older window preserved) |

## 10. Failure windows W1–W8 (live vs reopened)

| # | Window | Live-process action/report (precise, ephemeral) | Reopened classification (evidence-only) |
|---|---|---|---|
| W1 | Journal **append** failure (INTENT/UNDO-INTENT) | "append failed before the rename gate — rename never attempted"; run stops | torn tail beyond coverage ⇒ per §4.1: forward INTENT/UNDO-INTENT torn tail ⇒ per §4.1 expected-record rules (INTENT-side torn ⇒ blocked-after-reopening; see R20/R24) |
| W2 | Journal **flush** failure (INTENT/UNDO-INTENT) | "intent recorded but not flush-durable — rename never attempted"; run stops | complete INTENT beyond coverage ⇒ **blocked** (ambiguous per §4.1) |
| W3 | Journal flush/append failure (RESULT/UNDO-RESULT) after a permitted rename | "rename effect real; outcome record uncertain"; run stops | complete RESULT ⇒ **outcome applies + durability caveat**; torn RESULT ⇒ **blocked** |
| W4 | Anchor **append** failure | "journal record complete but unanchored"; next op forbidden; run stops | complete uncovered INTENT ⇒ **blocked**; complete uncovered RESULT ⇒ outcome+caveat |
| W5 | Anchor **flush** failure | same as W4; anchor bytes present/torn | coverage = last complete anchor record; uncovered(J) ⇒ per §4.1; empty ⇒ non-blocking |
| W6 | Torn anchor record/tail (crash) | — | coverage = last complete anchor record; uncovered(J) empty ⇒ debris disclosed; uncovered(J) non-empty ⇒ **blocked** |
| W7 | Journal **shorter** than anchor coverage | — | **blocked** (truncation/regression; lost range named) |
| W8 | Journal **longer** than anchor coverage | — | interpreted via the enforced stop-after-anchor-failure protocol: ≤1 uncovered record ⇒ classified per §4.1 (complete INTENT ⇒ blocked; complete RESULT ⇒ outcome+caveat); torn beyond coverage ⇒ per §4.1; >1 uncovered record ⇒ protocol violation ⇒ blocked |

END-specific: RUN-END/UNDO-END are unanchored terminals — their append/flush failures
classify per §7 (terminal rules), and the **fold, not END presence, decides candidates**.

## 11. Preview semantics

Best-effort **concurrent scanning — never an atomic snapshot**; per-entry facts as of
each entry's own open instant [SRC — the opened handle is the source of truth]; escape
list (post-open changes, late entries, rename ambiguity) — no universal-detection claim;
lock-state probe (held / never-created / free); journal-tail interpretation: lock held ⇒
"in progress, tail may be incomplete" (never corruption while a writer is live); lock
free ⇒ the §4.1/§10 classifications; read-only evidence inspection for blocked chains
(R4); **structural inability** of preview paths to write journals or rename files
(audit-enforced).

## 12. Capacity (derived arithmetic — exact canonical serialization is the runtime authority)

**Record counts:** forward = `1 + N + N + 1 = 2N + 2` (3,002 at N = 1,500) · anchor =
`1 + N + N = 2N + 1` (3,001; RUN-END not anchored) · attempt = `1 + c + c + 1 = 2c + 2`
(3,002).

**Per-record maxima (canonical serialization at `bc520c2`, hosted-verified; stored
record bytes including the terminating LF; JSON bytes excluding LF are one less).
Every journal/segment record carries `prev` (74 B: comma + `"prev":"` + 64 hex + `"`);
anchors carry none; undo records carry no `move_id`:** RUN-START 4,831 · INTENT 2,163 ·
RESULT 2,718 · RUN-END 411 · UNDO-START 529 · UNDO-INTENT 2,326 · UNDO-RESULT 2,881 ·
UNDO-END 537 · anchor 401. These are schema maxima (maximal run ≤34, 20-digit
`root_run_seq`/`undo_attempt_seq`/timestamps, 4,096-byte `root_path`, 765-byte names,
512-byte `detail`); hosted CI measured the T-15 fixtures at 4,831/2,142/2,697/369/508/
2,305/2,860/476/401 (fixture widths sit below the schema maxima). The section's earlier
derived estimates predate the implemented canonical form and are superseded. The
binding runtime cap remains the 8 KiB per-record limit (§5) — these maxima are
measurements, not new runtime restrictions.

**Per-file upper bounds — two bases, both ≤ 16 MiB.**

*Basis A — per-record maximum composition* (the doc's original method: every record
simultaneously at its maximum, including 20-digit `root_run_seq`/`undo_attempt_seq` and
8-digit seq/move/count widths):

| File | Formula | N = c = 1,500 |
|---|---|---|
| Forward journal | `4,831 + N×(2,163+2,718) + 411` = **`4,881N + 5,242 B`** | 7,326,742 B ≈ 6.99 MiB |
| Anchor | `(2N+1)×401` = **`802N + 401 B`** | 1,203,401 B ≈ 1.15 MiB |
| Attempt segment (unanchored) | `529 + c×(2,326+2,881) + 537` = **`5,207c + 1,066 B`** | 7,811,566 B ≈ 7.45 MiB |
| **One full cycle** | sum = **16,341,709 B** | ⇒ 8 cycles = 130,733,672 ≤ 134,217,728; headroom 3,484,056 B |

*Basis B — legal reserve-helper sequences* (what `forward_reserve_bytes` /
`segment_reserve_bytes` / `anchor_reserve_bytes` actually build: seq = record position
≤ 3,001, move/attempt/undo ids ≤ 1,499 and summary counts ≤ 1,500 ⇒ ≤4 digits;
anchor_seq/covered_seq = 2N ⇒ 4 digits):

| File | Formula | N = c = 1,500 |
|---|---|---|
| Forward journal | `4,831 + N×(2,155+2,710) + 387` = **`4,865N + 5,218 B`** | 7,302,718 B |
| Anchor | `(2N+1)×393` = **`786N + 393 B`** | 1,179,393 B |
| Attempt segment (unanchored) | `529 + c×(2,314+2,869) + 513` = **`5,183c + 1,042 B`** | 7,775,542 B |
| **One full cycle** | sum = **16,257,653 B** | ⇒ 8 cycles = 130,061,224 ≤ 134,217,728; headroom 4,156,504 B |

Basis A is the upper bound over the per-record ceilings; Basis B is the exact file
maximum the runtime admission arithmetic (the reserve helpers) builds under legal
sequence widths. Both preserve the ratified 8-cycle invariant.

**Three separated quantities:** theoretical per-file maximum (forward bound ⇒
**N ≈ 3,436**: (16,777,216 − 5,242)/4,881) · operational bound (**1,500 [SETTLED]**) ·
aggregate admission (exact byte recompute under the root lock at every operation start;
repeated authorized attempts each add one segment file and are admitted only when the
recomputed aggregate, including the new segment's exact worst case, fits — refusal
**before mutation** otherwise; no automatic deletion). The 8 KiB per-record cap alone
proves nothing about file compliance — the exact reserve check is the guarantee. No disk
pre-allocation; real exhaustion ⇒ flush failure ⇒ R1/R3.

## 13. Security and identity

Threat model: **accidental** misidentification and corruption in scope; same-privilege
adversaries **out** (profile-ACL boundary; integrity hashes ≠ tamper protection).
Gating fingerprint = volume + file ID + creation time — corroborating evidence, **not
mathematical proof** of historical identity; confounders named (file-ID reuse after
delete+recreate, `SetFileTime` mutability, NTFS tunneling, timestamp granularity).
Decision table: fingerprint matches (id + ctime) ⇒ proceed with disclosures; id matches,
ctime differs ⇒ presumed reuse ⇒ fail closed; id absent/changed ⇒ fail closed; **any
insufficient confidence ⇒ fail closed, overwrite nothing**. Size/mtime/content are
supplementary, non-gating (in-place modification is allowed and disclosed).
**Supported filesystems:** NTFS required for journaled operations; identity-less
filesystems never journaled [SRC: scanner `Unsupported` skip]; ReFS unverified
[ASSUMPTION]. **Undo = location reversal only — never content restoration; no merges; no
deletions.**

## 14. Preview semantics (single authoritative section)

Best-effort **concurrent scanning — never an atomic snapshot**; per-entry facts as of
each entry's own open instant [SRC — the opened handle is the source of truth]; escape
list: entries created after their enumeration slot may be absent; renames may appear
under either name; attribute/content changes **after** an entry's open are invisible to
that preview — no universal-detection claim. Lock-state probe annotates "operation in
progress" when the mutation lock is held; journal-tail interpretation: lock held ⇒ torn
tail = "in progress, tail may be incomplete" (never corruption while a writer is live);
lock free ⇒ the §4.1/§10 classifications. Read-only evidence inspection is always
available for blocked chains. **Structural inability to mutate:** preview paths contain
no journal-write and no rename capability; audit-enforced.

## 15. Failure and recovery rules (consolidated)

**R1** stop on write/flush failure after a mutation-capable point; next operation
forbidden until both confirmations · **R2** unterminated-but-complete = resolved,
disclosed, undo-eligible · **R3** END classification per §7; journals never rewritten
[G6] · **R4** identity/location evidence for **reporting only**; never drives mutation
or resolves state · **R5** failed reverse moves retried only via a new user-authorized
attempt · **R6** unresolved/blocked/ambiguous chain blocks forward apply, flag-immune;
ambiguous-segment chains stay forward-blocked (reconciliation excepted, §9.3) · **R7**
no automated unblock; `--recover` deferred [G7] · **R8** no success inference from
ambiguity; no automatic-recovery promise · **R9** inspection read-only · **R10** forward
abandoned-init non-blocking [G2] · **R11** `lock_id`/`anchor_id`/`journal_id`/
`fwd_journal_digest` continuity ⇒ refuse (identity binding is validated before
coverage arithmetic W7/R15 — the implemented and regression-tested order; both
refusals block) · **R12** segments/anchors are permanent
evidence; an ambiguous segment blocks its chain for forward · **R13** attempts admitted
pre-mutation on capacity · **R14** abandoned-init excluded from selection, sequences
consumed · **R15** journal shorter than anchor coverage ⇒ blocked, range named ·
**R16 [precise]** resurrected candidates: reconciliation may prove restoration
(destination fingerprint) and exclude them; if attempted, the reverse rename fails
`SourceMissing` (source absent after restoration) or `Conflict` (no-overwrite) — **an
unsafe repeated reversal is structurally impossible** (T-73) · **R17** ack activation at
the first **anchored** INTENT; loss of that anchor ⇒ chain blocked with "supersession
status unknown" — neither dormant nor preserved · **R18** anchors permanent evidence;
missing/empty anchor classified per §4.1 · **R19** at most **one** uncovered record is
protocol-reachable; classification is **type-and-state-aware** per §4.1 — a complete
uncovered INTENT is **blocked**, never a benign stop-tail · **R20** torn-tail
classification is expected-record-aware; the blanket inert-debris rule is withdrawn ·
**R21** RUN-END/UNDO-END are unanchored terminal records · **R22** anchor initialization
per §4; `anchor_id`/`journal_id` cross-binding mandatory · **R23** `attempt_candidate_count`
makes segment torn-END terminal-ness decidable · **R24** live-process failure reports are
precise but ephemeral; post-restart classification is evidence-only · **R25** coordinated
journal+anchor rollback to a mutually consistent older prefix is **undetectable** —
accepted threat-model boundary; resurrected candidates fail safe; disclosed as "history
may be longer than the surviving files show".

## 16. Ratified gates and decisions

| # | Decision | Status | Evidence requirement |
|---|---|---|---|
| G1 | SHA-256 provider: **BCrypt** | **CLOSED 2026-11-14** — hosted run `43598561201` (commit `dea0dc6`) green: Build (Debug)/(Release) linked `-lbcrypt`; Test (Debug)/(Release) executed the four NIST FIPS 180-4 vectors (empty, `abc`, 448-bit, 896-bit multi-block) green. Verified: bcrypt linkage (Build steps) and vector execution+passing (Test steps, exit-0 semantics) in both configurations. Not verified: per-vector pass counts (job log is auth-only); local execution (SAC ON). The G1-status comment in `platform/sha256.hpp` is updated at 7.1b. No in-tree fallback was needed | run `43598561201` job evidence (API) + Actions page |
| G2 | Forward abandoned-init non-blocking | **RATIFIED** | tests T-45…47 |
| G3 | Ack-flag name `--acknowledge-undo-window`; safety refusals exit 2, operational 3 | **RATIFIED** | tests T-01…07 |
| G4 | ACL **validation** (not construction) of the journals directory | **RATIFIED** | tests T-29…30 |
| G5 | Report wording + SAFETY invariants 13–18 + `executor.hpp` guarantee amendment | **DEFERRED to 7.4** | docs review |
| G6 | END no-rewrite policy (no repair passes) | **RATIFIED** | test T-65 |
| G7 | `--recover` deferred (blocked roots stay fail-closed until a designed recovery exists) | **RATIFIED** | — |
| G8 | Anchor protocol + expected-record-aware classification (§4–§7) | **RATIFIED** | tests T-60…70 |
| G9 | Attempt segments unanchored + reconciliation exception (§9.3) | **RATIFIED** | tests T-54…62b |
| G10 | No external checkpoint subsystem — conservative blocking is sufficient; coordinated rollback accepted as R25 | **RATIFIED** | §7 disclosure |
| G11 | Decision A: journals-directory ACL validator refuses absent and NULL DACLs; empty DACL denied through AccessCheck | **RATIFIED 2026-11-17** (owner; ChatGPT-delegated) | dedicated Decision-A test case (NULL-DACL validator refusal; absent/NULL/empty classification; real AccessCheck denial via pre-opened READ_CONTROL handle) |
| G12 | Decision B: root-key formula retained without creation time | **RATIFIED 2026-11-17** | residual documented in §13 |
| G13 | Decision C: hidden/system rule retained (reject on FolderPilot-created components, exempt the base ancestor) | **RATIFIED 2026-11-17** | T-18/T-19 analogues |
| G14 | Decision D: operability ACL validation retained; no principal-level enforcement claimed | **RATIFIED 2026-11-17** | §3 note |
| G15 | Decision E: lock_id derivation = SHA-256 of canonical lock-file identity material (conditions confirmed: deterministic canonicalization, stable identity binding, fail-closed) | **RATIFIED 2026-11-17** | RUN-START recording + cross-session verify land in 7.2 |

**Ratification order:** G8 → G9/G10 → G2/G6/G7 → G1 (evidence) → G3/G4/G5 (before 7.4).

## 17. Implementation milestones (each ends at a report gate)

| Step | Content | Gate |
|---|---|---|
| 7.1 | Journal module (deep module, C++20): canonical serializer, SHA-256 via BCrypt with hosted link probe + known-answer test, bounds enforcement; pure unit tests. **7.1a done: probe landed in `dea0dc6`; G1 closed (run `43598561201`, §16). 7.1b done: journal core landed (`b52179c` + remediation through `bc520c2`), hosted CI green (run `43924678919`: Debug and Release 200/200 cases, 13,843/13,843 assertions)** **7.1c done: storage adapter landed (`2c252e5` + `c1d38d0` + `e98089f`), hosted CI green (run `44341177181`: Debug and Release each 225/225 cases, 14,112/14,112 assertions, zero failed/skipped; artifact SHA-256 verified byte-level; genuine cross-process lock tests pass locally and hosted)** | 7.1b report |
| 7.2 | Forward-executor journal integration (write-ahead) + undo planning + reconciliation | Report |
| 7.3 | Undo execution + undo report + exit codes | Report |
| 7.4 | CLI surface (`--undo`, ack flag), reporter, **G5 wording + executor.hpp amendment + docs** | Report |
| 7.5 | Full local runtime verification (authorized SAC-off window) | Report |
| 7.6 | Hosted CI evidence + closeout | Acceptance |

**Documentation amendments due at 7.4 [G5]:** `lib/folderpilot/executor.hpp:9-14` — the
guarantee "never invents destination names, never re-plans, never retries, never deletes,
never rolls back" is amended to: within a single operation the executor still never
retries or rolls back on its own; **undo is a separate, explicitly user-invoked mode**
(`--undo --apply`) that performs fully revalidated reverse moves; the journal records
outcomes but is never trusted without revalidation. Also: `docs/ARCHITECTURE.md` (§1 row,
new §12 Phase 7 notes), `docs/SAFETY.md` (invariants 13–18: journal-is-data-never-trusted;
undo dry-run default; no-overwrite both directions; write-ahead durability; journal
outside the organized tree; plus R25 disclosure), `README.md` (undo usage).

## 18. Acceptance-test matrix (complete, self-contained)

| ID | Test | Layer | Key assertions |
|---|---|---|---|
| T-01…04 | CLI grammar: `--undo` preview; `--undo --apply` executes; `--apply` forward; unknown option/missing/duplicate root ⇒ exit 1 | CLI | §2 |
| T-05…07 | Ack flag: forward-only parser rules; supersession report precedes mutation; flag-immune blocked chains ⇒ exit 2 | CLI+Int | §2/§8 |
| T-08…10 | Record round-trips (all classes); unpaired handling; strict UTF-8 | Unit | fail-closed |
| T-11…14 | Torn final record; `seq`/`anchor_seq` gaps; unknown `v`/`type`; correlation contradictions ⇒ invalid/blocked | Unit | §5 |
| T-15…16 | SHA-256 NIST FIPS 180-2 vectors; canonical-serialization stability | Unit | G1 |
| T-17 | Identity comparator decision table (volume×id×ctime gating; size/mtime/content supplementary) — exhaustive | Unit | §13 |
| T-18…20 | Storage chain: relative creation; component reparse refusal; hidden `AppData` ancestor accepted | Integration | §3 |
| T-21…23 | Leaf reparse safety (lock/journal/segment/anchor): refuse, target untouched; `FILE_OPEN_IF`+flag created-vs-opened; `FILE_CREATE` refuse-on-exists | Integration | §3 |
| T-24…27 | Lock atomicity; probe triple (held/free/never-created); name collision; reader/writer sharing compatibility | Integration | §3 |
| T-28…30 | `lock_id` continuity; `<root-key>` swap ⇒ refuse; ACL validation failure ⇒ refuse | Integration | R11, G4 |
| T-31…32 | Exclusive second instance refused; crash-release then clean acquisition | Integration | §3 |
| T-33…36 | Forward apply + content checks; dry-run immutability; conflict preserves both sides; replaced/vanished/locked isolation | Integration | Phase 5 preserved |
| T-37 | Root changed ⇒ abort before mutation, exit 2 | Integration | — |
| T-38…40 | States A/B/C classification via `planned_move_count` + anchored-intent analysis | Integration | §6.3 |
| T-41…44 | Ack: durable+anchored before mutation; dormancy (RUN-START-only ⇒ older window preserved); activation at first anchored INTENT; **first-INTENT anchor loss ⇒ chain blocked with "supersession status unknown"** | Integration | §8, R17 |
| T-45…46 | Forward abandoned-init (zero records; torn head uniquely identifying RUN-START — untyped or differently-typed torn heads block): sequence consumed; byte-identical; non-blocking; disclosed | Integration | §6.3, G2 |
| T-47 | Declared-inert preserved older undo window (forward allowed without flag) | Integration | §6.3/§9.5 |
| T-48 | Unresolved/blocked chain ⇒ forward refused, flag-immune, exit 2 | Integration | R6 |
| T-49 | Zero-candidate window ⇒ forward allowed with disclosure; no segment for empty undo | Integration | §9.1/§12 |
| T-50 | RUN-START exact capacity reserve ⇒ refusal before the first INTENT when exceeded | Integration | §12 |
| T-51 | Undo preview: candidates + cannot-undo reasons; blocked-chain read-only evidence | Integration | §11/§14 |
| T-52…54 | Undo execute: location restoration + content checks; refilled slot ⇒ per-move Conflict; destination ctime mismatch ⇒ fail closed | Integration | §13 |
| T-55 | In-place modification (fingerprint match) ⇒ proceeds, differences disclosed | Integration | §13 |
| T-56…57 | Deleted journal ⇒ "nothing to undo"; corrupt journal ⇒ blocked | Integration | R7 |
| T-58 | Double undo ⇒ "nothing eligible", no segment | Integration | §9.1 |
| T-59 | Complete flushed UNDO-INTENT without RESULT ⇒ **unresolved/blocked** (reverse rename may have been attempted; flush terminology) | Integration | §6.2/§10 |
| T-60 | Reopened torn INTENT beyond coverage ⇒ **blocked (not inert)**; live-observed torn append reported precisely before restart | Integration | §4.1, R20/R24 |
| T-61 | Torn RESULT after covered INTENT ⇒ blocked (outcome unknown) | Integration | §4.1 |
| T-62 | Reopened torn/missing UNDO-INTENT ⇒ **blocked (not inert)**; complete UNDO-INTENT without RESULT ⇒ blocked | Integration | §6.2/§10 |
| T-62b | Complete UNDO-RESULT (restored) ⇒ fold removes candidate; durability caveat disclosed | Integration | §6.2 |
| T-63 | Torn terminal RUN-END/UNDO-END with all move records final ⇒ terminal rules (A / undo-complete-unterminated), disclosed | Integration | §7, R23 |
| T-64 | Impossible transitions (RESULT-before-INTENT, records after RUN-END, duplicate types) ⇒ protocol violation, blocked | Integration | §4.1 |
| T-65 | END complete-unanchored ⇒ closed-with-durability-caveat; **never rewritten** (byte-identity across later operations) | Integration | G6 |
| T-66…68 | Anchor initialization order (six steps); init crash windows (empty-journal/no-anchor; both-empty; RUN-START-unanchored ⇒ declared-inert; missing anchor with INTENTs ⇒ blocked); `anchor_id`/`journal_id` cross-binding ⇒ refuse on swap | Integration | §4 |
| T-69 | Anchor ordering: rename forbidden until INTENT anchored; next move forbidden until RESULT anchored | Integration | §4 |
| T-70 | W7/W8: journal-below-coverage ⇒ blocked; **one complete uncovered INTENT ⇒ blocked**; one complete uncovered RESULT ⇒ outcome+caveat; two uncovered records ⇒ protocol violation | Integration | §4.1, R19 |
| T-70b | Torn anchor tail: uncovered(J) empty ⇒ debris disclosed; uncovered(J) non-empty ⇒ blocked | Integration | §4.1 |
| T-71 | Anchor missing with INTENTs ⇒ blocked; without INTENTs ⇒ declared-inert + anchor-absent disclosure | Integration | §4.1 |
| T-72 | Anchor permanence across all operations (never deleted/rewritten) | Audit | R18 |
| T-73 | Reverse rename succeeds ⇒ segment suffix lost/truncated ⇒ candidate resurrects ⇒ reconciliation classifies (restored/eligible/unknown) ⇒ later attempt yields `SourceMissing`/`Conflict`, **zero mutation** | Integration | R16 |
| T-73b | Reconciliation decision table: dest-match+source-free ⇒ eligible; source-match+dest-absent ⇒ already-restored (removed); both/mismatch/replacement ⇒ refuse; **non-atomic ⇒ immediate revalidation before the reverse rename** | Integration | §9.3 |
| T-74…76 | Forward crash windows (W1/W2 live-precise reports; reopened ⇒ blocked); preview during apply (best-effort, torn-tail annotation); structural preview non-mutation audit | Integration/Audit | §10/§14 |
| T-77…79 | Multiple roots: per-root-key isolation; wrong root ⇒ identity fail-closed; older run never selected while newest chain exists/blocked | Integration | §12 |
| T-80 | 1,500-move bound + 128 MiB aggregate refusal (anchors counted), naming the storage path, no deletion | Integration | §12 |
| T-81 | Journals/anchors never inside the organized tree; scans never see storage | Integration | §3 |
| T-82 | Unicode + space paths end-to-end (journal, segment, report round-trip) | CLI | — |
| T-83 | Full Phase 1–6 regression: 144 cases, 1,762 assertions, zero failures/skips, Debug+Release | Suite | — |

## 19. Evidence and assumptions

**[W32-DOC]** (documented behavior, runtime-verified for in-tree directory/leaf patterns;
leaf-file/lock usage verified at 7.5): `FILE_OPEN_IF`/`FILE_OPEN_REPARSE_POINT` semantics
and sharing-conflict arithmetic; `FlushFileBuffers` GENERIC_WRITE requirement; kernel
handle cleanup at termination; `SetFileTime` mutability; NTFS tunneling; hidden `AppData`
attribute; filename-component character prohibitions; written-bytes-survive-process-
death. **[SRC]** scanner identity/`Unsupported` skip; opened-handle authority; platform-
only Win32 layering; 144/1,762 local and hosted CI evidence. Storage-adapter runtime findings (2026-11-18): `AccessCheck` requires an impersonation-level token (ERROR_BAD_IMPERSONATION_LEVEL against a primary token) and rejects group-less descriptors (ERROR_INVALID_SECURITY_DESCR) — the ACL validator therefore requests DACL|OWNER|GROUP and uses a Query-level impersonation duplicate; sharing checks exempt attributes-only access (FILE_READ_DATA required on the lock masks); `FileAttributeTagInfo`/identity queries require FILE_READ_ATTRIBUTES. **[VERIFIED]** MinGW bcrypt
import: hosted run `43598561201` built and linked `-lbcrypt` on `windows-latest` (G1
closed, §16). **[ASSUMPTIONS]** `%LOCALAPPDATA%` ACL defaults; ReFS identity behavior;
SAC-off window grantable for 7.5.

---

## 20. Phase 7.2 implementation record — journal-session integration, undo planning, reconciliation [RATIFIED 2026-11-18, delegated authority]

#### 20.1 Storage layout and naming [RATIFIED]

Per-root directory `<root-key>/` contains exactly: `mutation.lock`,
`journal-<R:020d>.jsonl`, `anchor-<R:020d>.jsonl`, `undo-<R:020d>-<A:020d>.jsonl`
— `R` = `root_run_seq`, `A` = `undo_attempt_seq`, both zero-padded 20-digit
decimal (lexicographic order ≡ numeric order). `run` = `run-<R>` (24 B ≤ 34
cap). `root_id` = the 64-hex root key (≤ 73 cap). One anchor per forward
journal. Segments unanchored [G9].

**Case-sensitivity policy [ADDENDUM; FILE_CREATE/FILE_OPEN_IF case-insensitive
matching via `OBJ_CASE_INSENSITIVE` — W32-DOC + SRC: platform/native.cpp]:**
storage directories are created by FolderPilot with default NTFS semantics
(case-insensitive); FolderPilot never enables per-directory case sensitivity.
`FILE_CREATE`/`FILE_OPEN_IF` opens are case-insensitive, so a case-variant of
a recognized name collides and cannot silently create a second file. Any
entry whose name does not canonicalize to a recognized pattern (in either
sensitivity regime) is an **unrecognized entry ⇒ refuse new mutation** (fail
closed). `.`/`..` enumeration pseudo-entries are skipped (quota parity) and
never treated as unrecognized. Case-variant or duplicate sequence identities,
malformed recognized-looking names (`journal-12a3.jsonl`, `journal-.jsonl`,
`anchor-x.jsonl`), and files that cannot be safely classified are all
unrecognized ⇒ refuse; **never choose between conflicting journals or
anchors**.

#### 20.2 Sequence allocation and storage classification [RATIFIED]

Under the mutation lock: enumerate `<root-key>`; classify every entry.
`root_run_seq` = max(recognized `journal-`/`undo-` first component) + 1.
Sequence gaps are legal evidence (abandoned-init consumed sequences per
§4/R14/§6.3) and never reused. Overflow beyond the 20-digit field cap ⇒
refuse new runs. Corrupt files classify per §4.1/§6.3 — corrupt newest chain
⇒ forward blocked (exit 2), undo per §9. Incomplete initialization (journal
without anchor, anchor without journal, zero-record journal) classifies per
§4/§6.3; a new run always allocates the **next** sequence. Journal
`FILE_CREATE` success followed by anchor `FILE_CREATE` failure ⇒ the run
fails, the sequence is consumed (abandoned-init on restart — safe, disclosed).
Failed `FILE_CREATE` (collision) ⇒ refuse. `undo_attempt_seq` allocation per
§6.2 lands with undo execution (7.3). **If the storage directory cannot be
classified safely, refuse new mutation.**

#### 20.3 Schemas [RATIFIED §5 unchanged]

No new record types, fields, or interpretations. Population conventions only:
INTENT/RESULT `file_ctime` = `creation_time_filetime`; `cat_id` = the
destination category directory's identity digest; `file_id` = the source
file's identity digest; `root_id` = root_key; `anchor_id`/`journal_id` per
§16 G15.

#### 20.4 Identity conventions [RATIFIED]

`E`/`L` are identity **encoding formats**, not cryptographic domain
separation. Digests (`lock_id`, `journal_id`, `anchor_id`, `cat_id`,
`file_id`) = lowercase-hex SHA-256 over the canonical representation-tagged
identity material (`'E'|'L'` tag + little-endian fixed-width fields);
`root_key` keeps its ratified G12 formula — SHA-256 over UTF-8(volume GUID
path) ‖ 0x00 ‖ the identity material. Purpose separation comes from what each
digest is **recorded against** (the specific object's identity; digests are
never compared cross-purpose — R11 continuity is per-field), pinned as
convention **v1** ratified herein (§20); G15 is precedent for lock_id; any
future change requires a ratified v2. Raw identity material never appears in
user-facing reports — only hex digests. `creation_time_filetime` =
`FILE_BASIC_INFO.CreationTime` (distinct from `ChangeTime`) is a **gating**
fingerprint component (§13): volume + file ID + creation time must **all**
match; id matches but ctime differs ⇒ presumed file-ID reuse ⇒ fail closed
(T-17, T-52…54); size/mtime/content remain supplementary, non-gating;
precision 100 ns; `SetFileTime` mutability and NTFS tunneling limitations
stand. Identity is captured through the opened handle (never path-only) and
revalidated before every mutation and during undo reconciliation; unknown
forms, capture failure, or mismatch fail closed.

#### 20.5 Capacity admission

Before any file creation: `check_aggregate_quota(root,
forward_reserve_bytes + anchor_reserve_bytes)` (exact worst-case recompute,
existing helpers, **under the mutation lock** per §12). Insufficient ⇒ refuse
**before the first mutation**, exit 3 — capacity exhaustion is operational
(environmental, user-remediable, no ambiguity), explicitly ratified here;
T-50/T-80 gain exit-code assertions. The storage path is named; no automatic
deletion. The 16 MiB per-file limit is enforced on every append (existing).

#### 20.6 Ordering and crash-consistency matrix

Per record: journal append → journal flush → anchor append → anchor flush →
next permitted operation. INTENT anchors before its rename; RESULT anchors
before the next move; RUN-START anchors before the first INTENT; RUN-END is an
unanchored terminal (flush only).

| # | Window | Durable journal state | Durable anchor state | Possible filesystem state | Restart classification | Mutation in interrupted run | Undo allowed | Owner intervention | Test |
|---|---|---|---|---|---|---|---|---|---|
| M0 | No journal created | none | none | none | nothing to classify | (run never started) | — | no | seq-allocation test (T-84) |
| M1 | Journal created, anchor `FILE_CREATE` failed | empty/uninitialized | missing | none | abandoned-forward-init (§4, G2/R14) | no (seq consumed) | per §9 | disclosed | init fail-inject (T-85) |
| M2 | RUN-START written, not flushed | torn RUN-START ⇒ abandoned-forward-init (§4.1/G2); complete unanchored RUN-START ⇒ declared-inert (§4) | missing | none | split per torn/complete | no | disclosed | no | journal-write fail-inject (T-86) |
| M3 | RUN-START flushed, genesis anchor not appended | complete RUN-START | missing/empty | none | declared-inert (§4) | no | disclosed | no | anchor-append fail-inject, W4 (T-87) |
| M4 | Genesis anchor appended, not flushed | RUN-START | torn genesis anchor tail | none | **declared-inert** (§4): covered RUN-START/no-INTENT if the genesis record survived complete; anchor-absent declared-inert disclosure otherwise — the implemented classifier (`journal_classify.cpp`) resolves the W6-vs-initialization tension as declared-inert at genesis | no (next op forbidden live) | n/a | no | anchor-flush fail-inject, W5 (T-88) |
| M5 | INTENT write failure (W1) | torn tail beyond coverage | unchanged | source intact | torn INTENT ⇒ **blocked** (§4.1/R20) | no | read-only evidence | no | journal-write fail-inject, W1 (T-89) |
| M6 | INTENT/anchor write or flush failure (W2/W4/W5) | complete uncovered INTENT | coverage excludes it | source intact | complete uncovered INTENT ⇒ **blocked** (R19; W2 journal-flush and W4/W5 anchor failures leave the same shape) | no | read-only evidence | no | anchor fail-inject (T-90) |
| M7 | INTENT anchored, rename not yet performed | covered INTENT | covers INTENT | source intact | dangling covered INTENT ⇒ **blocked** | no | read-only evidence | no | restart-matrix test (T-91) |
| M8 | Rename done, RESULT not written | covered INTENT | covers INTENT | file moved | same blocked (outcome unknown) | no | read-only evidence | no | restart-matrix test (T-91) |
| M9 | RESULT write/flush failure (W3) | covered INTENT + torn/uncovered RESULT | varies | moved | torn uncovered RESULT ⇒ blocked; complete uncovered RESULT ⇒ outcome + caveat (§4.1/W3) | no | read-only evidence | no | W3 fail-inject (T-92) |
| M10 | RESULT anchored | covered through RESULT | covers RESULT | moved | classified by outcome (state A/B semantics) | next move permitted | per §9 | no | restart-matrix test (T-91) |
| M11 | RUN-END partial/failed | complete move set + torn terminal END | unchanged | moved | terminal rules (§7, R2) | resolved; disclosed | eligible (fold) | no | W3 fail-inject (T-93) |
| M12 | Quota exhaustion at open | unchanged | unchanged | unchanged | refusal before any file creation | no (exit 3) | n/a | no | T-50 analogue + exit-code assertion (T-94) |
| M13 | Storage handle loss / termination | written prefix survives process death; flush-confirmed prefix survives power loss | same | last anchored rename persists | evidence-only classification (R24) | per classification | per classification | if blocked | termination tests (T-95) |

Forward-run windows only; attempt-segment windows (UNDO-INTENT/UNDO-RESULT,
§10/§6.2, T-59/T-62/T-62b) land with 7.3. Forward apply **after restart**
follows §9.5: a declared-inert or abandoned-init newest chain permits forward
apply with disclosure (G2/R10, T-45…47). No automatic recovery is promised;
unanchored/ambiguous states keep the ratified blocking semantics.

#### 20.7 Implementation specification

**W1** — `platform::open_source`/`inspect_source` gain
`creation_time_filetime` (from the already-fetched `FILE_BASIC_INFO`);
`ScannedFile` and `PlannedMove` propagate it; `StorageLeafOpen`/`StorageLeaf`
gain `id_hex` (digest of the captured identity via the G15 convention).
**W2** — `journal_session` (deep module): owns StorageRoot + MutationLock +
journal/anchor leaf handles; `open_for_run` (classification, allocation,
quota, `FILE_CREATE`×2, identity capture, RUN-START + genesis anchor),
`record_intent`, `record_result`, `end_run`; every failure = structured stop
with the durable-state classification. **W3** — `execute_plan` journaled path
in this explicit order: (1) explicit `--apply` authorization; (2) mutation
lock acquired; (3) storage root established + capacity reserved; (4) root
revalidation; (5) category revalidation/creation; (6) session `open_for_run`
(RUN-START written, flushed, genesis-anchored); (7) per move: INTENT written +
flushed + anchored; (8) per move: source/destination revalidation; (9) per
move: no-replace rename; (10) per move: RESULT written + flushed + anchored;
(11) RUN-END written + flushed (unanchored terminal) and release. The session
exists **only** in the `--apply` branch — the preview path has **no
journal-write capability and no rename capability by construction** (type
separation; §11/§14 audit test asserts BOTH inabilities). **W4** —
`undo_plan`: newest-chain selection by name enumeration + ratified
parsing/classification/fold; output classes Undoable / AlreadyReversed /
Conflict / Missing / Replaced / Ambiguous / Blocked / Corrupt — **every class
other than Undoable/AlreadyReversed is a refusal**; strictly read-only.

**§18 additions ratified herein:** storage-classification refusal tests
(case-variant names, malformed recognized-looking names, duplicate/conflicting
sequence identities, never-choose-between-conflicts); matrix window tests
**T-84…T-95** (M0–M13, table above); T-50/T-80 exit-code assertions; preview
dual-inability audit; quota-under-lock ordering assertion.

#### 20.8 Exit-code mapping [G3]

Safety refusals (blocked chains, ambiguous state, unrecognized storage) ⇒
exit 2. **CLI usage errors remain exit 1 per §2/T-01…04** (unknown option,
missing/duplicate root, malformed combinations). Operational failures
(capacity exhaustion — exit 3 explicitly ratified in §20.5 — storage I/O,
journal-write stops after a successful start) ⇒ exit 3. Previews and
successful runs ⇒ 0.

---

**Claim discipline:** this contract states only what the cited evidence shows or what the
ratified decisions define. Hosted CI capability is asserted only where runs verified it;
G1 is closed with hosted evidence (run `43598561201`); coordinated rollback is disclosed as
undetectable rather than hidden; and no guarantee exceeds what the implemented mechanism
will provide.
