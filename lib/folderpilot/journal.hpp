// FolderPilot - Phase 7 move journal: canonical records, strict parsing,
// chain verification and reopened-state classification (pure logic).
//
// This module implements docs/PHASE7-DESIGN.md sections 4-12 as pure
// functions over bytes already in memory: no files, no directories, no
// locking, no Win32. Storage (leaf protocol, locking) arrives with the
// storage adapter; executor integration (write-ahead ordering, undo
// planning) is 7.2+.
//
// Guarantees (exactly as implemented - no stronger claims):
//   * canonical JSONL serialization: fixed key order per record type,
//     strict UTF-8, minimal escaping, LF-terminated lines. The parser
//     rejects anything a ratified writer could never produce: unknown,
//     missing, misordered or duplicate keys, non-canonical numbers and
//     escape forms, oversize records and fields, unknown types/versions,
//     wrong value types,
//   * every journal/segment record carries `integrity` = SHA-256 over its
//     canonical bytes excluding the `integrity` member, and `prev` =
//     SHA-256 over the previous record's full canonical bytes (genesis =
//     SHA-256 over root-key || root_run_seq || run). Interior integrity and
//     ordering only - NEITHER the hash chain nor `seq` detects suffix
//     truncation (docs section 5),
//   * the reopened-state classification (docs 4.1/6.3/6.4/7/10, gate G8) is
//     fail-closed: every ambiguity resolves to blocked,
//   * no exceptions cross this API; results are ok/error structs.
//
// Pinned conventions (byte-level details the ratified design leaves open;
// self-consistent here, reported for ratification in the 7.1b report):
//   * hash inputs exclude the terminating LF. This is forced by R2's
//     unterminated-but-complete rule: only an LF-free hash can validate a
//     record whose final LF was never written,
//   * `prev` chains over the previous record's full canonical bytes
//     including its `integrity` member,
//   * an anchor record's `last_record_hash` is the covered journal record's
//     own `integrity` value,
//   * genesis seed = UTF-8(root_key) || UTF-8(decimal root_run_seq) ||
//     UTF-8(run), plain concatenation,
//   * canonical key order per record type exactly as serialized below,
//   * `outcome`/`detail` exist on RESULT/UNDO-RESULT only. Arithmetic
//     forces this: a maximal INTENT with both serializes to ~2.7 KiB,
//     contradicting section 12's own INTENT ceiling of 2,200 B, while one
//     without them serializes to ~2.17 KiB (matching the documented ~2,167
//     derivation). An INTENT has no outcome by definition,
//   * a torn tail that does not reveal its `type` classifies as blocked
//     (fail-closed), as does a torn INTENT beyond coverage (docs 4.1/R20/
//     T-60 govern; the section 6.3 "RUN-START + torn-INTENT tail" clause is
//     treated as withdrawn residue),
//   * a clean segment end after k complete candidates (k+1 <
//     attempt_candidate_count) classifies as partial-unambiguous, mirroring
//     forward state B's write-before-rename proof.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "folderpilot/category.hpp"

namespace folderpilot {

// ---------------------------------------------------------------------------
// Bounds (docs/PHASE7-DESIGN.md sections 5 and 12).
//
// Field caps are canonical CONTENT bytes (what sits between the quotes).
// The per-record cap covers the whole serialized line including its
// terminating LF. Hashes are SHA-256 via platform/sha256.hpp. The per-record
// ceilings derived in docs section 12 are documentation, not runtime caps:
// the runtime caps are the field caps below plus the 8 KiB record cap (the
// 7.1b report lists where the measured maximal records differ from the
// document's derived undo-record ceilings).
// ---------------------------------------------------------------------------

inline constexpr std::size_t journal_max_record_bytes = 8 * 1024;
inline constexpr std::size_t journal_max_file_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t journal_max_aggregate_bytes = 128 * 1024 * 1024;

// Operational move bound [SETTLED, docs section 12]: a run never plans more
// than 1,500 moves and an attempt never carries more candidates; recorded
// counts above it are invalid.
inline constexpr unsigned long long journal_move_limit = 1500;

inline constexpr std::size_t journal_run_max_bytes = 34;
// lock_id / root_id / anchor_id / journal_id / cat_id / file_id: opaque
// storage-verified identity strings. Formats are pinned where they are
// produced (storage adapter / executor integration); this module validates
// charset, escaping and length only.
inline constexpr std::size_t journal_identity_max_bytes = 73;
inline constexpr std::size_t journal_volume_max_bytes = 60;
inline constexpr std::size_t journal_root_path_max_bytes = 4096;
// Filenames are single components; they cannot contain '"', '\' or control
// characters [W32-DOC], so their canonical form never needs escapes and the
// 255-UTF-16-unit maximum is 765 UTF-8 bytes.
inline constexpr std::size_t journal_name_max_bytes = 765;
inline constexpr std::size_t journal_detail_max_bytes = 512;
inline constexpr std::size_t journal_hex_chars = 64;

// The exact source_name rules enforced by append_move_evidence (both a single
// component and a root-relative recursive path). Exposed so the planner can
// refuse an unjournalable source BEFORE the first mutation of a run.
bool validate_journal_source_name(std::string& error, std::string_view name);

// 8-digit fields: seq, move_id, attempt_move_seq, undo_of_move, the
// planned/candidate counts and the summary counts.
inline constexpr unsigned long long journal_seq_max = 99999999ULL;
// 20-digit fields span the full unsigned 64-bit range: root_run_seq,
// supersedes_root_run_seq, undo_attempt_seq, file_ctime, file_size,
// file_mtime, anchor byte_len.

// ---------------------------------------------------------------------------
// Record vocabulary.
// ---------------------------------------------------------------------------

// Direction recorded in every journal/segment record (`dir`).
enum class JournalDir { Forward, Undo };

// Record types (`type`). Serialized spellings: "RUN-START", "INTENT",
// "RESULT", "RUN-END", "UNDO-START", "UNDO-INTENT", "UNDO-RESULT",
// "UNDO-END". Anchor records carry `kind:"anchor"` instead of `type`.
enum class JournalRecordType {
    RunStart,
    Intent,
    Result,
    RunEnd,
    UndoStart,
    UndoIntent,
    UndoResult,
    UndoEnd,
};

std::string_view journal_record_type_name(JournalRecordType type);   // "" outside range

// Per-record outcomes (`outcome`). The forward names mirror the executor's
// MoveOutcome spelling one-to-one (lib/folderpilot/executor.hpp); mapping
// helpers belong with the executor integration (7.2+). `Restored` is the
// undo success outcome (serialized "restored", docs section 9.1) and is
// legal only in UNDO-RESULT records.
enum class JournalOutcome {
    Moved,
    Conflict,
    SourceMissing,
    SourceReplaced,
    SourceUnsafe,
    Locked,
    AccessDenied,
    DestinationUnsafe,
    DestinationChanged,
    CrossVolume,
    DestinationInvalid,
    SourceInvalid,
    Failed,
    Restored,
};

std::string_view journal_outcome_name(JournalOutcome outcome);       // "" outside range
std::optional<JournalOutcome> journal_outcome_from_name(std::string_view name);

// ---------------------------------------------------------------------------
// Record payloads (plain value structs; the wire format adds `prev` and
// `integrity`, which the serializer computes and the parser verifies - they
// are deliberately not part of the payload structs).
// ---------------------------------------------------------------------------

struct JournalRunStart {
    std::string run;                       // opaque run identifier, <=34 bytes
    unsigned long long seq = 0;            // always 0 (first record)
    unsigned long long root_run_seq = 0;
    unsigned long long planned_move_count = 0;   // <= journal_move_limit
    std::string lock_id;                   // identities, <=73 bytes each
    std::string root_id;
    std::string anchor_id;                 // cross-binding: names the anchor file
    std::string volume;                    // <=60 bytes, e.g. "\\?\Volume{...}\"
    std::string root_path;                 // <=4,096 bytes; backslash escaping counted
    // Durable acknowledgement pair (docs section 8). Serialized only when
    // forward --apply passed a live undo window via --acknowledge-undo-window:
    // then undo_window_acknowledged = true and supersedes_root_run_seq is
    // present. Absent otherwise. An explicit `false` is rejected by the
    // parser (a ratified writer never emits it).
    bool undo_window_acknowledged = false;
    unsigned long long supersedes_root_run_seq = 0;
    bool has_ack = false;                  // true iff the pair is serialized

    bool operator==(const JournalRunStart&) const = default;
};

struct JournalIntent {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    unsigned long long move_id = 0;        // 0-based, contiguous within the run
    std::string cat_id;                    // destination category directory identity
    std::string file_id;                   // source file identity (fingerprint component)
    unsigned long long file_ctime = 0;     // gating fingerprint component (FILETIME)
    unsigned long long file_size = 0;      // supplementary
    unsigned long long file_mtime = 0;     // supplementary (FILETIME)
    std::string source_name;               // forward-recorded component names
    std::string dest_name;
    Category dest_cat{};                   // serialized as the category folder name

    bool operator==(const JournalIntent&) const = default;
};

struct JournalResult {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    unsigned long long move_id = 0;
    std::string cat_id;
    std::string file_id;
    unsigned long long file_ctime = 0;
    unsigned long long file_size = 0;
    unsigned long long file_mtime = 0;
    std::string source_name;
    std::string dest_name;
    Category dest_cat{};
    JournalOutcome outcome{};              // forward outcomes only (no Restored)
    std::string detail;                    // <=512 content bytes; may be empty

    bool operator==(const JournalResult&) const = default;
};

struct JournalRunEnd {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    // Summary counts mirroring ExecutionResult (executor.hpp):
    // planned/moved/conflict/skipped/failed.
    unsigned long long planned_count = 0;
    unsigned long long moved_count = 0;
    unsigned long long conflict_count = 0;
    unsigned long long skipped_count = 0;
    unsigned long long failed_count = 0;

    bool operator==(const JournalRunEnd&) const = default;
};

struct JournalUndoStart {
    std::string run;                       // the forward run this segment undoes
    unsigned long long seq = 0;            // always 0 (first record)
    unsigned long long root_run_seq = 0;   // the forward run's sequence
    std::string fwd_journal_digest;        // 64 hex; constant per segment
    unsigned long long undo_attempt_seq = 0;   // per-root counter; filename ordering
    std::string journal_id;                // identity of the forward journal file
    unsigned long long attempt_candidate_count = 0;   // >= 1, <= journal_move_limit

    bool operator==(const JournalUndoStart&) const = default;
};

struct JournalUndoIntent {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    std::string fwd_journal_digest;
    unsigned long long undo_attempt_seq = 0;
    unsigned long long attempt_move_seq = 0;   // 0-based, contiguous within the segment
    unsigned long long undo_of_move = 0;       // forward move_id being reversed
    std::string cat_id;
    std::string file_id;
    unsigned long long file_ctime = 0;
    unsigned long long file_size = 0;
    unsigned long long file_mtime = 0;
    std::string source_name;               // forward-recorded names; the reverse
    std::string dest_name;                 // rename moves dest_name back to source_name
    Category dest_cat{};

    bool operator==(const JournalUndoIntent&) const = default;
};

struct JournalUndoResult {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    std::string fwd_journal_digest;
    unsigned long long undo_attempt_seq = 0;
    unsigned long long attempt_move_seq = 0;
    unsigned long long undo_of_move = 0;
    std::string cat_id;
    std::string file_id;
    unsigned long long file_ctime = 0;
    unsigned long long file_size = 0;
    unsigned long long file_mtime = 0;
    std::string source_name;
    std::string dest_name;
    Category dest_cat{};
    JournalOutcome outcome{};              // full vocabulary including Restored
    std::string detail;

    bool operator==(const JournalUndoResult&) const = default;
};

struct JournalUndoEnd {
    std::string run;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    std::string fwd_journal_digest;
    unsigned long long undo_attempt_seq = 0;
    // Summary counts in the same key order as RUN-END. For a segment,
    // moved_count counts successful restorations (docs section 9.1: outcome
    // "restored"); the wording review at 7.4 [G5] revisits the labels.
    unsigned long long planned_count = 0;
    unsigned long long moved_count = 0;
    unsigned long long conflict_count = 0;
    unsigned long long skipped_count = 0;
    unsigned long long failed_count = 0;

    bool operator==(const JournalUndoEnd&) const = default;
};

// Anchor record (`kind:"anchor"`). Anchors the forward journal only (docs
// section 4; attempt segments are unanchored in Phase 7 [G9]). No `prev`
// chain: anchor records carry their own `integrity` and are ordered by
// `anchor_seq`; coverage claims are ordered by `covered_seq`/`byte_len`.
struct AnchorRecord {
    unsigned long long root_run_seq = 0;
    std::string journal_id;                // identity of the anchored journal
    unsigned long long anchor_seq = 0;     // 0-based, gap-free
    unsigned long long covered_seq = 0;    // last covered journal record's seq
    unsigned long long byte_len = 0;       // journal bytes covered (LFs included)
    std::string last_record_hash;          // 64 hex = covered record's `integrity`

    bool operator==(const AnchorRecord&) const = default;
};

using JournalRecord = std::variant<JournalRunStart, JournalIntent, JournalResult, JournalRunEnd,
                                   JournalUndoStart, JournalUndoIntent, JournalUndoResult,
                                   JournalUndoEnd>;

// ---------------------------------------------------------------------------
// Serialization. All results are ok/error structs; hashing failures (the
// BCrypt wrapper) surface as !ok with the native error text.
// ---------------------------------------------------------------------------

struct JournalSerializeResult {
    bool ok = false;
    std::string canonical;   // the LF-terminated canonical line (or file)
    std::string integrity_hex;  // this record's own integrity (the anchor's
                                // last_record_hash); single-record form only
    std::string error;       // meaningful when !ok
};

struct JournalHashResult {
    bool ok = false;
    std::string hex;         // 64 lowercase hex characters
    std::string error;       // meaningful when !ok
};

// Genesis `prev` seed for the first record of a journal or segment file:
// SHA-256 over UTF-8(root_key) || UTF-8(decimal root_run_seq) || UTF-8(run).
[[nodiscard]] JournalHashResult journal_genesis_prev(std::string_view root_key,
                                                     unsigned long long root_run_seq,
                                                     std::string_view run);

// Serializes one journal record to its canonical LF-terminated line using
// the given `prev` value (64 hex). Enforces every field cap and the record
// cap. `prev` for the first record is the genesis seed.
[[nodiscard]] JournalSerializeResult serialize_journal_record(const JournalRecord& record,
                                                              std::string_view prev_hex);

// Convenience: serializes a whole record sequence into one journal/segment
// byte image, computing the genesis seed and the prev chain. An empty
// sequence yields empty bytes (the abandoned-initialization file state).
[[nodiscard]] JournalSerializeResult serialize_journal_chain(std::span<const JournalRecord> records,
                                                             std::string_view root_key);

// Serializes one anchor record to its canonical LF-terminated line. Anchor
// records have no prev; their integrity is computed like every record's.
[[nodiscard]] JournalSerializeResult serialize_anchor_record(const AnchorRecord& record);

// Lowercase hex encoding; used for digests and comparisons.
[[nodiscard]] std::string journal_hex(std::span<const unsigned char> bytes);

// ---------------------------------------------------------------------------
// Strict parsing (whole byte images already in memory).
// ---------------------------------------------------------------------------

enum class ParseFailureKind {
    OversizeFile,           // input exceeds journal_max_file_bytes
    InvalidUtf8,
    NonCanonicalEncoding,   // whitespace, wrong escape/number forms, key order
    DuplicateKey,
    UnknownField,
    WrongValueType,
    MissingField,
    CapExceeded,            // field caps, record cap, file cap, move limit
    UnknownTypeOrVersion,
    ChainBreak,             // prev/integrity mismatch (incl. genesis seed)
    TransitionViolation,    // expected-record rules; impossible order
    ContinuityMismatch,     // run/root_run_seq/dir/fwd_journal_digest discontinuity
    CorrelationAnomaly,     // ack pairing, outcome vocabulary, move-id pairing
    HashUnavailable,        // the SHA-256 provider failed during verification
};

struct ParseFailure {
    ParseFailureKind kind{};
    std::size_t offset = 0;    // byte offset into the input where detection happened
    std::string detail;
};

// Tail status of a parsed file. CleanEof: the image ends with a terminated
// record (or is empty). Torn: trailing bytes after the last complete record
// do not form a valid record (crash mid-write or corruption). A final
// record written without its LF but otherwise fully valid is COMPLETE
// (R2's unterminated-but-complete rule) and is reported through
// unterminated_final_record instead.
enum class JournalTailKind { CleanEof, Torn };

struct ParsedJournalFile {
    bool valid = false;                    // schema, caps, chain, transitions all pass
    ParseFailure failure{};                // meaningful when !valid
    std::vector<JournalRecord> records;    // complete records, in file order
    std::vector<std::size_t> record_end;   // one-past-the-end byte offset per record
    std::vector<std::array<unsigned char, 32>> record_digests;  // per-record `integrity`
    std::size_t total_bytes = 0;
    JournalTailKind tail = JournalTailKind::CleanEof;
    std::vector<unsigned char> tail_bytes;             // Torn only
    std::optional<JournalRecordType> tail_type_hint;   // Torn only; set iff exactly one
                                                       // known type prefix-matches
    bool unterminated_final_record = false;            // R2 disclosure
};

// Parses a forward journal or an undo-attempt segment (both are JSONL record
// streams; `dir` distinguishes them). root_key is the storage root-key used
// in the genesis `prev` seed; it is required - fail-closed.
[[nodiscard]] ParsedJournalFile parse_journal_file(std::span<const unsigned char> bytes,
                                                   std::string_view root_key);

struct ParsedAnchorFile {
    bool valid = false;
    ParseFailure failure{};
    std::vector<AnchorRecord> records;
    std::vector<std::size_t> record_end;
    std::size_t total_bytes = 0;
    JournalTailKind tail = JournalTailKind::CleanEof;
    std::vector<unsigned char> tail_bytes;
    bool unterminated_final_record = false;
};

// Parses an anchor file image. Anchor records are self-integrity-checked
// and order-checked (anchor_seq gap-free from 0, covered_seq contiguous
// ascending from 0, byte_len strictly increasing).
[[nodiscard]] ParsedAnchorFile parse_anchor_file(std::span<const unsigned char> bytes);

// ---------------------------------------------------------------------------
// Reopened-state classification (docs sections 4.1, 6.3, 6.4, 7, 10; G8).
// Pure functions over the parsed files; every ambiguity resolves blocked.
// ---------------------------------------------------------------------------

enum class ForwardChainState {
    Closed,                  // complete RUN-END (+ durability caveat)
    CompleteUnterminated,    // state A: every planned move final, END absent/torn-terminal
    PartialUnambiguous,      // state B: k of N finals, no INTENT beyond the prefix
    Blocked,                 // state C / protocol violation / anchor / W7 refusals
    DeclaredInert,           // RUN-START only, zero INTENT bytes (docs section 6.3)
    AbandonedForwardInit,    // zero complete records; a torn head qualifies only
                             // when it uniquely identifies RUN-START (docs
                             // sections 4/6.3, G2 + the pinned torn-tail rule)
};

enum class ForwardBlockReason {
    ParseInvalid,
    JournalBelowCoverage,         // W7: journal shorter than anchor coverage
    DanglingCoveredIntent,        // covered INTENT without RESULT: rename attempted, outcome unknown
    TornIntentBeyondCoverage,     // docs 4.1/R20/T-60
    CompleteIntentBeyondCoverage, // W2/W4 ambiguity (R19)
    TornResultBeyondCoverage,     // W3: outcome unknown
    MultipleUncoveredRecords,     // W8/R19: more than one uncovered record
    TornAnchorTailWithContent,    // W6: torn/failed anchor tail while uncovered(J) non-empty
    AnchorMissingWithIntents,     // missing/empty anchor, INTENT present (docs 4.1)
    ProtocolViolation,            // impossible anchor claims or record shapes
    TransitionViolation,
    ChainBreak,
    IdentityMismatch,             // anchor_id/journal_id cross-binding refusal (R11)
    UncoveredTailUntyped,         // torn tail too short to reveal its type (fail-closed)
    CorrelationAnomaly,
};

// Docs section 8: activation at the first successfully anchored INTENT.
// Dormant: acknowledgement recorded, no anchored INTENT. Active: at least
// one anchored INTENT. Unknown: the chain is blocked, so the supersession
// status cannot be proven either way.
enum class AckWindowStatus { None, Dormant, Active, Unknown };

struct ForwardChainClassification {
    ForwardChainState state{};
    ForwardBlockReason reason{};     // meaningful when state == Blocked
    std::string detail;              // human-readable evidence (named W7 range etc.)
    std::size_t completed_moves = 0; // final RESULT count (states A/B; Closed: N)
    std::optional<JournalOutcome> uncovered_result_outcome;  // W3/W4 complete RESULT
    bool durability_caveat = false;  // unanchored/unterminated evidence (R2, docs section 7)
    bool anchor_absent = false;      // declared-inert anchor-absent disclosure
    bool torn_anchor_debris = false; // torn anchor tail with uncovered(J) empty (T-70b)
    bool torn_terminal_end = false;  // torn terminal RUN-END/UNDO-END disclosure
    bool unterminated_final_record = false;
    AckWindowStatus ack_window = AckWindowStatus::None;
};

struct ChainIdentityInputs {
    std::string expected_journal_id;   // storage-verified identity of the journal file
    std::string expected_anchor_id;    // storage-verified identity of the anchor file
};

// Classifies a reopened forward journal against its anchor. `anchor` may be
// null (anchor file missing - byte-level view; an existing-but-empty anchor
// file classifies identically with the same disclosure).
[[nodiscard]] ForwardChainClassification classify_forward_chain(
    const ParsedJournalFile& journal,
    const ParsedAnchorFile* anchor,
    const ChainIdentityInputs& ids);

enum class SegmentState {
    Closed,                  // complete UNDO-END (+ durability caveat)
    CompleteUnterminated,    // all attempt_candidate_count candidates final,
                             // END absent or torn-terminal: the fold decides
    PartialUnambiguous,      // k complete UNDO-RESULTs, k+1 < candidate_count,
                             // clean end (ratification pending - see report)
    Blocked,
    AbandonedAttemptInit,    // zero complete records / torn UNDO-START head (docs 6.4)
};

enum class SegmentBlockReason {
    ParseInvalid,
    DanglingUndoIntent,           // complete flushed UNDO-INTENT without RESULT (T-59)
    TornUndoIntent,               // T-62
    TornUndoResult,               // W3-segment
    TornTerminalEndInconsistent,  // torn END while candidates are not all final
    TransitionViolation,
    ChainBreak,
    UncoveredTailUntyped,
    CorrelationAnomaly,
};

struct SegmentClassification {
    SegmentState state{};
    SegmentBlockReason reason{};      // meaningful when state == Blocked
    std::string detail;
    std::size_t completed_candidates = 0;
    bool durability_caveat = false;   // segments are never anchored [G9]
    bool torn_terminal_end = false;
    bool unterminated_final_record = false;
};

// Classifies a reopened undo-attempt segment (unanchored; docs 6.4/10).
[[nodiscard]] SegmentClassification classify_segment(const ParsedJournalFile& segment);

struct BindingCheck {
    bool ok = false;
    std::string error;   // meaningful when !ok
};

// Verifies the segment's constant `fwd_journal_digest` against the forward
// journal byte image it claims to undo (docs section 5).
[[nodiscard]] BindingCheck verify_segment_digest(const ParsedJournalFile& segment,
                                                 std::span<const unsigned char> forward_journal_bytes);

// ---------------------------------------------------------------------------
// Capacity (docs section 12). Exact canonical serialization is the runtime
// authority; these helpers compute byte-exact worst cases.
// ---------------------------------------------------------------------------

struct CapacityEstimate {
    bool ok = false;
    std::size_t bytes = 0;   // exact worst-case canonical size incl. LFs
    std::string error;       // meaningful when !ok
};

// Exact canonical byte count (LFs included) of the given records. Each
// record must serialize cleanly (a fixed dummy prev occupies the same bytes
// as any real one); a failure is reported, never silently under-counted.
[[nodiscard]] CapacityEstimate total_journal_bytes(std::span<const JournalRecord> records);

// Worst-case forward journal size for a planned run (pre-mutation reserve
// check): the real RUN-START plus maximal INTENT/RESULT pairs and a maximal
// RUN-END. planned_move_count must be within journal_move_limit.
[[nodiscard]] CapacityEstimate forward_reserve_bytes(const JournalRunStart& start,
                                                     unsigned long long planned_move_count);

// Worst-case attempt-segment size for a planned undo.
[[nodiscard]] CapacityEstimate segment_reserve_bytes(const JournalUndoStart& start,
                                                     unsigned long long candidate_count);

// Worst-case anchor size for a forward run: 2N+1 maximal anchor records
// (RUN-END is never anchored).
[[nodiscard]] CapacityEstimate anchor_reserve_bytes(unsigned long long planned_move_count);

inline bool within_per_file_limit(std::size_t bytes) {
    return bytes <= journal_max_file_bytes;
}

inline bool within_aggregate_limit(std::size_t existing_bytes, std::size_t proposed_reserve) {
    return existing_bytes + proposed_reserve <= journal_max_aggregate_bytes;
}

} // namespace folderpilot
