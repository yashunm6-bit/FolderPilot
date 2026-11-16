// FolderPilot - Phase 7.1b journal chain-classification unit tests: the G8
// reopened-state table (docs/PHASE7-DESIGN.md sections 4.1, 6.3, 6.4, 7, 10)
// as pure functions over bytes in memory.
//
// Each TEST_CASE names its T-xx ID from the design's acceptance matrix
// (section 18). Integration-layer re-verification of the same scenarios
// happens when the storage adapter and executor land (7.1c/7.2+).

#include <doctest.h>

#include "folderpilot/journal.hpp"
#include "sha256.hpp"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace folderpilot;

constexpr unsigned long long kRootRunSeq = 7;
constexpr const char* kRun = "0123456789abcdef0123456789abcdef";
constexpr const char* kRootKey = "root-key";
// Synthetic identity material, consistent between the RUN-START's anchor_id,
// the anchor's journal_id and the identity inputs.
const std::string kIdentity(73, 'i');
const ChainIdentityInputs kIds{kIdentity, kIdentity};

std::string hex64(char c = 'a') {
    return std::string(journal_hex_chars, c);
}

std::vector<unsigned char> to_bytes(const std::string& s) {
    return std::vector<unsigned char>(s.begin(), s.end());
}

JournalRunStart make_run_start(unsigned long long planned = 2, bool ack = false) {
    JournalRunStart rs;
    rs.run = kRun;
    rs.root_run_seq = kRootRunSeq;
    rs.planned_move_count = planned;
    rs.lock_id = kIdentity;
    rs.root_id = kIdentity;
    rs.anchor_id = kIdentity;
    rs.volume = "\\\\?\\Volume{01234567-8901-2345-6789-0123456789ab}\\";
    rs.root_path = "C:\\Users\\demo\\Desktop";
    rs.undo_window_acknowledged = ack;
    rs.supersedes_root_run_seq = 3;
    rs.has_ack = ack;
    return rs;
}

JournalIntent make_intent(unsigned long long seq, unsigned long long move_id) {
    JournalIntent r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.move_id = move_id;
    r.cat_id = kIdentity;
    r.file_id = kIdentity;
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    return r;
}

JournalResult make_result(unsigned long long seq, unsigned long long move_id) {
    JournalResult r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.move_id = move_id;
    r.cat_id = kIdentity;
    r.file_id = kIdentity;
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    r.outcome = JournalOutcome::Moved;
    r.detail = "";
    return r;
}

JournalRunEnd make_run_end(unsigned long long seq, unsigned long long planned) {
    JournalRunEnd r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.planned_count = planned;
    r.moved_count = planned;
    return r;
}

JournalUndoStart make_undo_start(unsigned long long candidates) {
    JournalUndoStart r;
    r.run = kRun;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = 1;
    r.journal_id = kIdentity;
    r.attempt_candidate_count = candidates;
    return r;
}

JournalUndoIntent make_undo_intent(unsigned long long seq, unsigned long long attempt_move) {
    JournalUndoIntent r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = 1;
    r.attempt_move_seq = attempt_move;
    r.undo_of_move = attempt_move;
    r.cat_id = kIdentity;
    r.file_id = kIdentity;
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    return r;
}

JournalUndoResult make_undo_result(unsigned long long seq, unsigned long long attempt_move) {
    JournalUndoResult r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = 1;
    r.attempt_move_seq = attempt_move;
    r.undo_of_move = attempt_move;
    r.cat_id = kIdentity;
    r.file_id = kIdentity;
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    r.outcome = JournalOutcome::Restored;
    r.detail = "";
    return r;
}

JournalUndoEnd make_undo_end(unsigned long long seq, unsigned long long candidates) {
    JournalUndoEnd r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = 1;
    r.planned_count = candidates;
    r.moved_count = candidates;
    return r;
}

ParsedJournalFile parse_journal_ok(const std::string& image) {
    ParsedJournalFile p = parse_journal_file(to_bytes(image), kRootKey);
    REQUIRE_MESSAGE(p.valid, "journal parse failed: ", p.failure.detail);
    return p;
}

ParsedAnchorFile parse_anchor_ok(const std::string& image) {
    ParsedAnchorFile p = parse_anchor_file(to_bytes(image));
    REQUIRE_MESSAGE(p.valid, "anchor parse failed: ", p.failure.detail);
    return p;
}

std::string ser_record(const JournalRecord& rec) {
    const JournalSerializeResult r = serialize_journal_record(rec, hex64());
    REQUIRE_MESSAGE(r.ok, "serialize failed: ", r.error);
    return r.canonical;
}

std::string ser_anchor(const AnchorRecord& rec) {
    const JournalSerializeResult r = serialize_anchor_record(rec);
    REQUIRE_MESSAGE(r.ok, "anchor serialize failed: ", r.error);
    return r.canonical;
}

// Serializes one record as record 0 of its file: `prev` is the ratified
// genesis seed (the parser verifies record 0 against it).
std::string ser_first(const JournalRecord& rec) {
    unsigned long long root_run_seq = 0;
    std::string run;
    std::visit([&](const auto& r) {
        root_run_seq = r.root_run_seq;
        run = r.run;
    }, rec);
    const JournalHashResult genesis = journal_genesis_prev(kRootKey, root_run_seq, run);
    REQUIRE(genesis.ok);
    const JournalSerializeResult r = serialize_journal_record(rec, genesis.hex);
    REQUIRE_MESSAGE(r.ok, "serialize failed: ", r.error);
    return r.canonical;
}

// Serializes a whole record sequence: the genesis seed and the prev chain
// are computed by the serializer, exactly as a writer would.
std::string chain_bytes(const std::vector<JournalRecord>& records) {
    const JournalSerializeResult r = serialize_journal_chain(records, kRootKey);
    REQUIRE_MESSAGE(r.ok, "chain serialize failed: ", r.error);
    return r.canonical;
}

// SHA-256 of a canonical line without its LF - the prev of the next record.
std::string prev_of(const std::string& line) {
    std::string_view view(line);
    if (!view.empty() && view.back() == '\n') {
        view.remove_suffix(1);
    }
    const platform::Sha256Result h = platform::sha256(to_bytes(std::string(view)));
    REQUIRE(h.ok);
    return journal_hex(h.digest);
}

// Byte offset of line `index` (0-based) within a serialized image.
std::size_t line_offset(const std::string& image, std::size_t index) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const std::size_t nl = image.find('\n', pos);
        REQUIRE(nl != std::string::npos);
        pos = nl + 1;
    }
    return pos;
}

// A parsed journal plus a consistent anchor image covering its first
// `anchor_count` records (one anchor record per journal record).
struct BuiltChain {
    std::string journal;
    std::string anchor;
    ParsedJournalFile parsed;
};

BuiltChain build_chain(const std::vector<JournalRecord>& records, std::size_t anchor_count) {
    BuiltChain out;
    const JournalSerializeResult chain = serialize_journal_chain(records, kRootKey);
    REQUIRE(chain.ok);
    out.journal = chain.canonical;
    out.parsed = parse_journal_ok(out.journal);
    REQUIRE(out.parsed.record_digests.size() == records.size());
    for (std::size_t i = 0; i < anchor_count && i < records.size(); ++i) {
        AnchorRecord a;
        a.root_run_seq = kRootRunSeq;
        a.journal_id = kIdentity;
        a.anchor_seq = i;
        a.covered_seq = i;
        a.byte_len = out.parsed.record_end[i];
        a.last_record_hash = journal_hex(out.parsed.record_digests[i]);
        out.anchor += ser_anchor(a);
    }
    return out;
}

// The classify entry point used by every forward test.
ForwardChainClassification classify(const BuiltChain& chain) {
    const ParsedAnchorFile a = chain.anchor.empty()
                                   ? ParsedAnchorFile{}
                                   : parse_anchor_ok(chain.anchor);
    const ParsedAnchorFile* anchor_ptr = chain.anchor.empty() ? nullptr : &a;
    return classify_forward_chain(chain.parsed, anchor_ptr, kIds);
}

ForwardChainClassification classify(const ParsedJournalFile& journal,
                                    const ParsedAnchorFile* anchor) {
    return classify_forward_chain(journal, anchor, kIds);
}

std::string cut_before(std::string_view image, std::string_view marker,
                       std::size_t keep_after_marker) {
    const std::size_t pos = image.find(marker);
    REQUIRE_MESSAGE(pos != std::string::npos, "marker not found: ", marker);
    return std::string(image.substr(0, pos + keep_after_marker));
}

} // namespace

// ---------------------------------------------------------------------------
// Forward chain.
// ---------------------------------------------------------------------------

TEST_CASE("T-45: forward abandoned initialization is non-blocking (G2)") {
    // Zero complete records: empty file.
    ParsedJournalFile empty = parse_journal_file({}, kRootKey);
    REQUIRE(empty.valid);
    const ForwardChainClassification c1 = classify(empty, nullptr);
    CHECK(c1.state == ForwardChainState::AbandonedForwardInit);
    CHECK(c1.anchor_absent);
    CHECK(c1.ack_window == AckWindowStatus::None);

    // Torn RUN-START head: still abandoned initialization, disclosed.
    const BuiltChain full = build_chain({JournalRecord{make_run_start(2, false)}}, 1);
    const std::string torn_head = cut_before(full.journal, "\"planned_move_count\":2", 2);
    const ParsedJournalFile p = parse_journal_ok(torn_head);
    CHECK(p.records.empty());
    CHECK(p.tail == JournalTailKind::Torn);
    const ForwardChainClassification c2 = classify(p, nullptr);
    CHECK(c2.state == ForwardChainState::AbandonedForwardInit);
    CHECK(c2.anchor_absent);
}

TEST_CASE("T-45: torn-head typing separates abandoned-init from blocked") {
    // Untyped torn head (cut inside the run string): blocked, never an
    // abandoned initialization (ratified fail-closed rule).
    const BuiltChain bare = build_chain({JournalRecord{make_run_start(2, false)}}, 1);
    const std::string short_head = bare.journal.substr(0, 14);  // `{"v":1,"run":"`
    const ParsedJournalFile untyped = parse_journal_ok(short_head);
    CHECK(untyped.records.empty());
    CHECK(untyped.tail == JournalTailKind::Torn);
    CHECK_FALSE(untyped.tail_type_hint.has_value());
    const ForwardChainClassification c1 = classify(untyped, nullptr);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::UncoveredTailUntyped);

    // Typed INTENT head: INTENT evidence exists without any anchor record.
    const std::string intent_head =
        cut_before(ser_first(JournalRecord{make_intent(1, 0)}), "\"type\":\"INTENT\"", 14);
    const ParsedJournalFile typed = parse_journal_ok(intent_head);
    REQUIRE(typed.tail_type_hint.has_value());
    CHECK(*typed.tail_type_hint == JournalRecordType::Intent);
    const ForwardChainClassification c2 = classify(typed, nullptr);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::AnchorMissingWithIntents);

    // An impossible first-record type (RESULT): protocol violation.
    const std::string result_head =
        cut_before(ser_first(JournalRecord{make_result(0, 0)}), "\"type\":\"RESULT\"", 14);
    const ParsedJournalFile impossible = parse_journal_ok(result_head);
    REQUIRE(impossible.tail_type_hint.has_value());
    CHECK(*impossible.tail_type_hint == JournalRecordType::Result);
    const ForwardChainClassification c3 = classify(impossible, nullptr);
    CHECK(c3.state == ForwardChainState::Blocked);
    CHECK(c3.reason == ForwardBlockReason::ProtocolViolation);

    // An undo-typed head in a forward journal is an impossible shape too.
    const std::string undo_head =
        cut_before(ser_first(JournalRecord{make_undo_start(1)}), "\"type\":\"UNDO-START\"", 14);
    const ParsedJournalFile undo_typed = parse_journal_ok(undo_head);
    REQUIRE(undo_typed.tail_type_hint.has_value());
    CHECK(*undo_typed.tail_type_hint == JournalRecordType::UndoStart);
    const ForwardChainClassification c3b = classify(undo_typed, nullptr);
    CHECK(c3b.state == ForwardChainState::Blocked);
    CHECK(c3b.reason == ForwardBlockReason::ProtocolViolation);

    // Empty journal + existing-but-empty anchor file: classifies identically
    // to a missing anchor, with the same anchor-absent disclosure.
    ParsedJournalFile empty = parse_journal_file({}, kRootKey);
    const ParsedAnchorFile empty_anchor = parse_anchor_file({});
    const ForwardChainClassification c4 = classify(empty, &empty_anchor);
    CHECK(c4.state == ForwardChainState::AbandonedForwardInit);
    CHECK(c4.anchor_absent);

    // An anchor file with only a torn tail (zero complete records) adds the
    // debris disclosure; the state is unchanged.
    AnchorRecord debris_head;
    debris_head.root_run_seq = kRootRunSeq;
    debris_head.journal_id = kIdentity;
    debris_head.anchor_seq = 0;
    debris_head.covered_seq = 0;
    debris_head.byte_len = 42;
    debris_head.last_record_hash = hex64('b');
    std::string torn_anchor_image = ser_anchor(debris_head);
    torn_anchor_image = torn_anchor_image.substr(0, torn_anchor_image.size() - 10);
    const ParsedAnchorFile torn_empty_anchor = parse_anchor_ok(torn_anchor_image);
    REQUIRE(torn_empty_anchor.records.empty());
    CHECK(torn_empty_anchor.tail == JournalTailKind::Torn);
    const ForwardChainClassification c4b = classify(empty, &torn_empty_anchor);
    CHECK(c4b.state == ForwardChainState::AbandonedForwardInit);
    CHECK(c4b.torn_anchor_debris);

    // Empty journal under an anchor claiming coverage: W7 blocks with the
    // lost range named.
    const ParsedAnchorFile a = parse_anchor_ok(bare.anchor);
    const ForwardChainClassification c5 = classify(empty, &a);
    CHECK(c5.state == ForwardChainState::Blocked);
    CHECK(c5.reason == ForwardBlockReason::JournalBelowCoverage);
    CHECK_MESSAGE(c5.detail.find("lost range") != std::string::npos,
                  "the lost range must be named: ", c5.detail);

    // Empty journal + foreign-identity anchor: the R11 refusal is what
    // reports (R11 precedes W7 in the zero-record path too).
    ChainIdentityInputs wrong_journal{kIdentity + "x", kIdentity};
    const ForwardChainClassification c6 =
        classify_forward_chain(empty, &a, wrong_journal);
    CHECK(c6.state == ForwardChainState::Blocked);
    CHECK(c6.reason == ForwardBlockReason::IdentityMismatch);

    // W7 precedence in the zero-record path (R15/W7: the journal is shorter
    // than the anchor's proven prefix, so the coverage refusal is what
    // reports - whatever the torn head's typing). The identity matches, so
    // R11 passes and W7 fires with the lost range named.
    const std::string rs_typed_head =
        cut_before(ser_first(JournalRecord{make_run_start(2, false)}),
                   "\"planned_move_count\":2", 2);
    for (const std::string& head_image : {short_head, rs_typed_head, intent_head}) {
        const ParsedJournalFile covered = parse_journal_ok(head_image);
        const ForwardChainClassification hc = classify(covered, &a);
        CHECK(hc.state == ForwardChainState::Blocked);
        CHECK(hc.reason == ForwardBlockReason::JournalBelowCoverage);
        CHECK_MESSAGE(hc.detail.find("lost range") != std::string::npos,
                      "the lost range must be named: ", hc.detail);
    }
}

TEST_CASE("T-66/T-70: R11 identity binding precedes W7 coverage when both fail") {
    const BuiltChain full =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ParsedAnchorFile a = parse_anchor_ok(full.anchor);
    const std::size_t proven = a.records.back().byte_len;
    REQUIRE(proven > 10);
    const std::string truncated = full.journal.substr(0, proven - 10);
    const ParsedJournalFile short_journal = parse_journal_ok(truncated);

    // Both wrong (foreign journal identity + journal below coverage): the
    // identity refusal is what reports - the implemented R11-first order,
    // pinned here.
    ChainIdentityInputs wrong_journal{kIdentity + "x", kIdentity};
    const ForwardChainClassification c1 =
        classify_forward_chain(short_journal, &a, wrong_journal);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::IdentityMismatch);

    // Identity correct + below coverage: the W7 refusal (the mirrored half
    // of the precedence pair).
    const ForwardChainClassification c2 =
        classify_forward_chain(short_journal, &a, kIds);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::JournalBelowCoverage);
    CHECK_MESSAGE(c2.detail.find("lost range") != std::string::npos,
                  "the lost range must be named: ", c2.detail);
}

TEST_CASE("T-47/section 8: dormant ack preserved on declared-inert runs") {
    // RUN-START only, no anchor: declared-inert with anchor-absent disclosure.
    const BuiltChain bare = build_chain({JournalRecord{make_run_start(2, true)}}, 0);
    const ForwardChainClassification c1 = classify(bare);
    CHECK(c1.state == ForwardChainState::DeclaredInert);
    CHECK(c1.anchor_absent);
    CHECK(c1.ack_window == AckWindowStatus::Dormant);

    // RUN-START covered by the genesis anchor record: still declared-inert.
    const BuiltChain covered = build_chain({JournalRecord{make_run_start(2, true)}}, 1);
    const ForwardChainClassification c2 = classify(covered);
    CHECK(c2.state == ForwardChainState::DeclaredInert);
    CHECK_FALSE(c2.anchor_absent);
    CHECK(c2.ack_window == AckWindowStatus::Dormant);
}

TEST_CASE("T-60: reopened torn INTENT beyond coverage blocks (not inert)") {
    const BuiltChain chain =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)}}, 1);
    // Genesis anchor covers RUN-START; the INTENT line is torn mid-record
    // (past the type field so the hint is typed).
    const std::string torn = cut_before(chain.journal, "\"type\":\"INTENT\"", 14);
    const ParsedJournalFile p = parse_journal_ok(torn);
    REQUIRE(p.tail_type_hint.has_value());
    CHECK(*p.tail_type_hint == JournalRecordType::Intent);
    const ParsedAnchorFile a = parse_anchor_ok(chain.anchor);
    const ForwardChainClassification c = classify(p, &a);
    CHECK(c.state == ForwardChainState::Blocked);
    CHECK(c.reason == ForwardBlockReason::TornIntentBeyondCoverage);

    // The same shape with a live acknowledgement window: supersession status
    // unknown (docs section 8, R17) because no INTENT is provably anchored.
    const BuiltChain acked =
        build_chain({JournalRecord{make_run_start(1, true)},
                     JournalRecord{make_intent(1, 0)}}, 1);
    const std::string torn2 = cut_before(acked.journal, "\"type\":\"INTENT\"", 14);
    const ParsedJournalFile p2 = parse_journal_ok(torn2);
    const ParsedAnchorFile a2 = parse_anchor_ok(acked.anchor);
    const ForwardChainClassification c2 = classify(p2, &a2);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::TornIntentBeyondCoverage);
    CHECK(c2.ack_window == AckWindowStatus::Unknown);
}

TEST_CASE("T-61: torn RESULT after a covered INTENT blocks") {
    const BuiltChain chain =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 2);
    const std::string torn = cut_before(chain.journal, "\"type\":\"RESULT\"", 14);
    const ParsedJournalFile p = parse_journal_ok(torn);
    REQUIRE(p.tail_type_hint.has_value());
    CHECK(*p.tail_type_hint == JournalRecordType::Result);
    const ParsedAnchorFile a = parse_anchor_ok(chain.anchor);
    const ForwardChainClassification c = classify(p, &a);
    CHECK(c.state == ForwardChainState::Blocked);
    CHECK(c.reason == ForwardBlockReason::TornResultBeyondCoverage);
}

TEST_CASE("T-70: W7/W8 coverage arithmetic (below coverage, INTENT, RESULT, two records)") {
    // W7: journal shorter than the anchor's proven prefix; the lost range is
    // named in the detail.
    const BuiltChain full =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ParsedAnchorFile a3 = parse_anchor_ok(full.anchor);
    const std::size_t proven = a3.records.back().byte_len;
    REQUIRE(proven > 10);
    const std::string truncated = full.journal.substr(0, proven - 10);
    const ParsedJournalFile short_journal = parse_journal_ok(truncated);
    const ForwardChainClassification c1 = classify(short_journal, &a3);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::JournalBelowCoverage);
    CHECK_MESSAGE(c1.detail.find("lost range") != std::string::npos,
                  "the lost range must be named: ", c1.detail);

    // W8/R19: one complete uncovered INTENT is blocked.
    const BuiltChain intent_uncovered =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)}}, 1);
    const ForwardChainClassification c2 = classify(intent_uncovered);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::CompleteIntentBeyondCoverage);

    // One complete uncovered RESULT: the outcome applies with a durability
    // caveat (W3/W4).
    const BuiltChain result_uncovered =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 2);
    const ForwardChainClassification c3 = classify(result_uncovered);
    CHECK(c3.state == ForwardChainState::CompleteUnterminated);
    REQUIRE(c3.uncovered_result_outcome.has_value());
    CHECK(*c3.uncovered_result_outcome == JournalOutcome::Moved);
    CHECK(c3.durability_caveat);
    CHECK(c3.completed_moves == 1);

    // More than one uncovered record: protocol violation.
    const BuiltChain two_uncovered =
        build_chain({JournalRecord{make_run_start(2, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)},
                     JournalRecord{make_intent(3, 1)}}, 2);
    const ForwardChainClassification c4 = classify(two_uncovered);
    CHECK(c4.state == ForwardChainState::Blocked);
    CHECK(c4.reason == ForwardBlockReason::MultipleUncoveredRecords);
}

TEST_CASE("T-70b: torn anchor tail - debris when uncovered(J) is empty, blocked otherwise") {
    // Empty: every journal record covered; a torn fourth anchor line is
    // harmless debris (reachable when an unflushed journal append's bytes
    // never survived while the anchor's torn bytes did).
    const BuiltChain full =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    AnchorRecord fourth;
    fourth.root_run_seq = kRootRunSeq;
    fourth.journal_id = kIdentity;
    fourth.anchor_seq = 3;
    fourth.covered_seq = 3;
    fourth.byte_len = 999999;
    fourth.last_record_hash = hex64('d');
    std::string debris_anchor = full.anchor + ser_anchor(fourth);
    debris_anchor = debris_anchor.substr(0, debris_anchor.size() - 10);  // torn tail
    const ParsedAnchorFile torn_anchor = parse_anchor_ok(debris_anchor);
    REQUIRE(torn_anchor.records.size() == 3);
    CHECK(torn_anchor.tail == JournalTailKind::Torn);
    const ForwardChainClassification c1 = classify(full.parsed, &torn_anchor);
    CHECK(c1.state == ForwardChainState::CompleteUnterminated);
    CHECK(c1.torn_anchor_debris);

    // Non-empty: the journal has a complete uncovered RESULT while the
    // anchor tail is torn - blocked (the torn tail may have covered an
    // INTENT whose anchoring is now unprovable).
    const BuiltChain partial =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 2);
    AnchorRecord third;
    third.root_run_seq = kRootRunSeq;
    third.journal_id = kIdentity;
    third.anchor_seq = 2;
    third.covered_seq = 2;
    third.byte_len = partial.parsed.record_end[2];
    third.last_record_hash = journal_hex(partial.parsed.record_digests[2]);
    std::string torn_anchor2 = partial.anchor + ser_anchor(third);
    torn_anchor2 = torn_anchor2.substr(0, torn_anchor2.size() - 10);
    const ParsedAnchorFile torn2 = parse_anchor_ok(torn_anchor2);
    REQUIRE(torn2.records.size() == 2);
    CHECK(torn2.tail == JournalTailKind::Torn);
    const ForwardChainClassification c2 = classify(partial.parsed, &torn2);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::TornAnchorTailWithContent);
}

TEST_CASE("T-71: missing anchor - INTENT evidence blocks, clean head is declared-inert") {
    const BuiltChain with_intent =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)}}, 0);
    const ForwardChainClassification c1 = classify(with_intent);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::AnchorMissingWithIntents);

    const BuiltChain head_only = build_chain({JournalRecord{make_run_start(2, false)}}, 0);
    const ForwardChainClassification c2 = classify(head_only);
    CHECK(c2.state == ForwardChainState::DeclaredInert);
    CHECK(c2.anchor_absent);
}

TEST_CASE("T-38/T-40 (unit basis): states A/B/C and Closed via planned_move_count") {
    // Closed: complete unanchored RUN-END.
    const BuiltChain closed =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)},
                     JournalRecord{make_run_end(3, 1)}}, 3);
    const ForwardChainClassification c1 = classify(closed);
    CHECK(c1.state == ForwardChainState::Closed);
    CHECK(c1.durability_caveat);
    CHECK(c1.completed_moves == 1);

    // State A: every planned move final, END absent, fully covered.
    const BuiltChain complete =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ForwardChainClassification c2 = classify(complete);
    CHECK(c2.state == ForwardChainState::CompleteUnterminated);
    CHECK(c2.completed_moves == 1);

    // State B: interrupted after k of N planned moves, nothing attempted
    // beyond the prefix.
    const BuiltChain partial =
        build_chain({JournalRecord{make_run_start(2, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ForwardChainClassification c3 = classify(partial);
    CHECK(c3.state == ForwardChainState::PartialUnambiguous);
    CHECK(c3.completed_moves == 1);
    CHECK_MESSAGE(c3.detail.find("never attempted") != std::string::npos,
                  "state B discloses the unattempted tail: ", c3.detail);

    // State C: dangling covered INTENT.
    const BuiltChain dangling =
        build_chain({JournalRecord{make_run_start(2, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)},
                     JournalRecord{make_intent(3, 1)}}, 4);
    const ForwardChainClassification c4 = classify(dangling);
    CHECK(c4.state == ForwardChainState::Blocked);
    CHECK(c4.reason == ForwardBlockReason::DanglingCoveredIntent);

    // planned_move_count == 0 with RUN-END: closed, dormant window.
    const BuiltChain empty_run =
        build_chain({JournalRecord{make_run_start(0, true)},
                     JournalRecord{make_run_end(1, 0)}}, 1);
    const ForwardChainClassification c5 = classify(empty_run);
    CHECK(c5.state == ForwardChainState::Closed);
    CHECK(c5.ack_window == AckWindowStatus::Dormant);
}

TEST_CASE("T-63: torn terminal RUN-END with all moves final is terminal and disclosed") {
    const BuiltChain chain =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)},
                     JournalRecord{make_run_end(3, 1)}}, 3);
    const std::string torn = cut_before(chain.journal, "\"type\":\"RUN-END\"", 14);
    const ParsedJournalFile p = parse_journal_ok(torn);
    REQUIRE(p.tail_type_hint.has_value());
    CHECK(*p.tail_type_hint == JournalRecordType::RunEnd);
    const ParsedAnchorFile a = parse_anchor_ok(chain.anchor);
    const ForwardChainClassification c = classify(p, &a);
    CHECK(c.state == ForwardChainState::CompleteUnterminated);
    CHECK(c.torn_terminal_end);
    CHECK(c.completed_moves == 1);

    // A torn RUN-END while planned RESULTs are missing is a protocol
    // violation, not a terminal state.
    const BuiltChain inconsistent =
        build_chain({JournalRecord{make_run_start(2, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)},
                     JournalRecord{make_run_end(3, 2)}}, 3);
    const std::string torn2 = cut_before(inconsistent.journal, "\"type\":\"RUN-END\"", 14);
    const ParsedJournalFile p2 = parse_journal_ok(torn2);
    const ParsedAnchorFile a2 = parse_anchor_ok(inconsistent.anchor);
    const ForwardChainClassification c2 = classify(p2, &a2);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::ProtocolViolation);
}

TEST_CASE("T-64 (unit basis): impossible transitions refuse at parse level") {
    // A forward journal that starts with a RESULT. The head carries a valid
    // genesis prev, so the transition rule is what refuses.
    const std::string image = ser_first(JournalRecord{make_result(0, 0)});
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(image), kRootKey);
    CHECK_FALSE(p1.valid);
    const ForwardChainClassification c1 = classify_forward_chain(p1, nullptr, kIds);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::ParseInvalid);

    // Records after the terminal RUN-END.
    const std::string end_image =
        chain_bytes({JournalRecord{make_run_start(0, false)},
                     JournalRecord{make_run_end(1, 0)}});
    const std::string last_line = end_image.substr(line_offset(end_image, 1));
    const std::string after =
        end_image +
        serialize_journal_record(JournalRecord{make_intent(2, 0)}, prev_of(last_line))
            .canonical;
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(after), kRootKey);
    CHECK_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::TransitionViolation);
}

TEST_CASE("T-66 (unit basis): anchor_id/journal_id cross-binding refuses on swap") {
    const BuiltChain chain =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)}}, 2);
    const ParsedAnchorFile a = parse_anchor_ok(chain.anchor);

    ChainIdentityInputs wrong_journal{kIdentity + "x", kIdentity};
    const ForwardChainClassification c1 = classify_forward_chain(chain.parsed, &a, wrong_journal);
    CHECK(c1.state == ForwardChainState::Blocked);
    CHECK(c1.reason == ForwardBlockReason::IdentityMismatch);

    ChainIdentityInputs wrong_anchor{kIdentity, std::string(73, 'z')};
    const ForwardChainClassification c2 = classify_forward_chain(chain.parsed, &a, wrong_anchor);
    CHECK(c2.state == ForwardChainState::Blocked);
    CHECK(c2.reason == ForwardBlockReason::IdentityMismatch);
}

TEST_CASE("T-41 to T-44 (unit basis): acknowledgement activation and dormancy") {
    // Anchored INTENT activates the window.
    const BuiltChain active =
        build_chain({JournalRecord{make_run_start(1, true)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ForwardChainClassification c1 = classify(active);
    CHECK(c1.state == ForwardChainState::CompleteUnterminated);
    CHECK(c1.ack_window == AckWindowStatus::Active);

    // RUN-START-only (covered): dormant, older window preserved.
    const BuiltChain dormant = build_chain({JournalRecord{make_run_start(1, true)}}, 1);
    const ForwardChainClassification c2 = classify(dormant);
    CHECK(c2.state == ForwardChainState::DeclaredInert);
    CHECK(c2.ack_window == AckWindowStatus::Dormant);

    // Complete uncovered first INTENT: blocked, supersession unknown (R17).
    const BuiltChain unknown =
        build_chain({JournalRecord{make_run_start(1, true)},
                     JournalRecord{make_intent(1, 0)}}, 1);
    const ForwardChainClassification c3 = classify(unknown);
    CHECK(c3.state == ForwardChainState::Blocked);
    CHECK(c3.reason == ForwardBlockReason::CompleteIntentBeyondCoverage);
    CHECK(c3.ack_window == AckWindowStatus::Unknown);

    // No acknowledgement pair at all: None even when INTENTs are anchored.
    const BuiltChain no_ack =
        build_chain({JournalRecord{make_run_start(1, false)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ForwardChainClassification c4 = classify(no_ack);
    CHECK(c4.ack_window == AckWindowStatus::None);
}

TEST_CASE("W7 evidence integrity: a blocked chain with anchored INTENTs reports Active") {
    // The chain is blocked below coverage, but the anchor proves INTENT 0
    // was anchored: the supersession window is Active, not Unknown.
    const BuiltChain full =
        build_chain({JournalRecord{make_run_start(1, true)},
                     JournalRecord{make_intent(1, 0)},
                     JournalRecord{make_result(2, 0)}}, 3);
    const ParsedAnchorFile a3 = parse_anchor_ok(full.anchor);
    const std::size_t proven = a3.records.back().byte_len;
    const std::string truncated = full.journal.substr(0, proven - 10);
    const ParsedJournalFile short_journal = parse_journal_ok(truncated);
    const ForwardChainClassification c = classify(short_journal, &a3);
    CHECK(c.state == ForwardChainState::Blocked);
    CHECK(c.reason == ForwardBlockReason::JournalBelowCoverage);
    CHECK(c.ack_window == AckWindowStatus::Unknown);  // W7: coverage broken
}

// ---------------------------------------------------------------------------
// Undo-attempt segment.
// ---------------------------------------------------------------------------

ParsedJournalFile parse_segment_ok(const std::string& image) {
    ParsedJournalFile p = parse_journal_file(to_bytes(image), kRootKey);
    REQUIRE_MESSAGE(p.valid, "segment parse failed: ", p.failure.detail);
    return p;
}

TEST_CASE("T-59: complete flushed UNDO-INTENT without UNDO-RESULT blocks") {
    const std::string image = chain_bytes({JournalRecord{make_undo_start(1)},
                                           JournalRecord{make_undo_intent(1, 0)}});
    const ParsedJournalFile seg = parse_segment_ok(image);
    const SegmentClassification c = classify_segment(seg);
    CHECK(c.state == SegmentState::Blocked);
    CHECK(c.reason == SegmentBlockReason::DanglingUndoIntent);
}

TEST_CASE("T-62: reopened torn UNDO-INTENT blocks; untyped tail blocks") {
    const std::string image = chain_bytes({JournalRecord{make_undo_start(1)},
                                           JournalRecord{make_undo_intent(1, 0)}});
    const std::string torn = cut_before(image, "\"type\":\"UNDO-INTENT\"", 14);
    const ParsedJournalFile p1 = parse_segment_ok(torn);
    REQUIRE(p1.tail_type_hint.has_value());
    CHECK(*p1.tail_type_hint == JournalRecordType::UndoIntent);
    const SegmentClassification c1 = classify_segment(p1);
    CHECK(c1.state == SegmentState::Blocked);
    CHECK(c1.reason == SegmentBlockReason::TornUndoIntent);

    // A tail too short to type is refused (fail-closed).
    const std::size_t first_lf = image.find('\n') + 1;
    const std::string short_tail = image.substr(0, first_lf + 8);
    const ParsedJournalFile p2 = parse_segment_ok(short_tail);
    CHECK(p2.tail == JournalTailKind::Torn);
    CHECK_FALSE(p2.tail_type_hint.has_value());
    const SegmentClassification c2 = classify_segment(p2);
    CHECK(c2.state == SegmentState::Blocked);
    CHECK(c2.reason == SegmentBlockReason::UncoveredTailUntyped);
}

TEST_CASE("T-62b: complete UNDO-RESULT (restored) resolves; T-63 segment torn END") {
    const std::string image = chain_bytes({JournalRecord{make_undo_start(1)},
                                           JournalRecord{make_undo_intent(1, 0)},
                                           JournalRecord{make_undo_result(2, 0)}});
    const SegmentClassification c1 = classify_segment(parse_segment_ok(image));
    CHECK(c1.state == SegmentState::CompleteUnterminated);
    CHECK(c1.completed_candidates == 1);
    CHECK(c1.durability_caveat);  // segments are never anchored [G9]

    // Torn terminal UNDO-END with all candidates final: the fold decides
    // (R23: attempt_candidate_count makes the terminal-ness decidable).
    const std::string full =
        chain_bytes({JournalRecord{make_undo_start(1)},
                     JournalRecord{make_undo_intent(1, 0)},
                     JournalRecord{make_undo_result(2, 0)},
                     JournalRecord{make_undo_end(3, 1)}});
    const std::string torn = cut_before(full, "\"type\":\"UNDO-END\"", 14);
    const ParsedJournalFile p2 = parse_segment_ok(torn);
    REQUIRE(p2.tail_type_hint.has_value());
    CHECK(*p2.tail_type_hint == JournalRecordType::UndoEnd);
    const SegmentClassification c2 = classify_segment(p2);
    CHECK(c2.state == SegmentState::CompleteUnterminated);
    CHECK(c2.torn_terminal_end);
    CHECK(c2.completed_candidates == 1);

    // Torn UNDO-END while candidate RESULTs are missing: inconsistent.
    const std::string two =
        chain_bytes({JournalRecord{make_undo_start(2)},
                     JournalRecord{make_undo_intent(1, 0)},
                     JournalRecord{make_undo_result(2, 0)},
                     JournalRecord{make_undo_end(3, 2)}});
    const std::string torn2 = cut_before(two, "\"type\":\"UNDO-END\"", 14);
    const SegmentClassification c3 = classify_segment(parse_segment_ok(torn2));
    CHECK(c3.state == SegmentState::Blocked);
    CHECK(c3.reason == SegmentBlockReason::TornTerminalEndInconsistent);
}

TEST_CASE("section 6.4: abandoned attempt initialization (torn/zero head)") {
    ParsedJournalFile empty = parse_journal_file({}, kRootKey);
    const SegmentClassification c1 = classify_segment(empty);
    CHECK(c1.state == SegmentState::AbandonedAttemptInit);

    const std::string head = ser_first(JournalRecord{make_undo_start(1)});
    const std::string torn = cut_before(head, "\"attempt_candidate_count\":1", 2);
    const SegmentClassification c2 = classify_segment(parse_segment_ok(torn));
    CHECK(c2.state == SegmentState::AbandonedAttemptInit);
    CHECK_MESSAGE(c2.detail.find("torn") != std::string::npos,
                  "torn head disclosed: ", c2.detail);

    // A torn head too short to reveal its type is blocked, never an
    // abandoned initialization (same ratified rule as the forward
    // classifier).
    const std::string short_head = head.substr(0, 14);  // `{"v":1,"run":"`
    const ParsedJournalFile p3 = parse_segment_ok(short_head);
    CHECK(p3.tail == JournalTailKind::Torn);
    CHECK_FALSE(p3.tail_type_hint.has_value());
    const SegmentClassification c3 = classify_segment(p3);
    CHECK(c3.state == SegmentState::Blocked);
    CHECK(c3.reason == SegmentBlockReason::UncoveredTailUntyped);

    // Typed evidence of a later undo operation without a valid start:
    // blocked as an impossible shape.
    const std::string intent_head =
        cut_before(ser_first(JournalRecord{make_undo_intent(1, 0)}),
                   "\"type\":\"UNDO-INTENT\"", 14);
    const ParsedJournalFile p4 = parse_segment_ok(intent_head);
    REQUIRE(p4.tail_type_hint.has_value());
    CHECK(*p4.tail_type_hint == JournalRecordType::UndoIntent);
    const SegmentClassification c4 = classify_segment(p4);
    CHECK(c4.state == SegmentState::Blocked);
    CHECK(c4.reason == SegmentBlockReason::TransitionViolation);
}

TEST_CASE("Q3 (ratification pending): clean partial segment end resolves as partial") {
    // Interrupted after candidate 0 of 2: candidates 1.. were never attempted.
    const std::string image = chain_bytes({JournalRecord{make_undo_start(2)},
                                           JournalRecord{make_undo_intent(1, 0)},
                                           JournalRecord{make_undo_result(2, 0)}});
    const SegmentClassification c1 = classify_segment(parse_segment_ok(image));
    CHECK(c1.state == SegmentState::PartialUnambiguous);
    CHECK(c1.completed_candidates == 1);
    CHECK_MESSAGE(c1.detail.find("never attempted") != std::string::npos,
                  "disclosure present: ", c1.detail);

    // Interrupted before the first candidate: same proof with k = 0.
    const std::string head = chain_bytes({JournalRecord{make_undo_start(2)}});
    const SegmentClassification c2 = classify_segment(parse_segment_ok(head));
    CHECK(c2.state == SegmentState::PartialUnambiguous);
    CHECK(c2.completed_candidates == 0);
}

TEST_CASE("T-73 (basis): verify_segment_digest binds the segment to the forward bytes") {
    const std::string forward =
        ser_record(JournalRecord{make_run_start(1, false)});
    const platform::Sha256Result h = platform::sha256(to_bytes(forward));
    REQUIRE(h.ok);

    JournalUndoStart us = make_undo_start(1);
    us.fwd_journal_digest = journal_hex(h.digest);
    const std::string segment = chain_bytes({JournalRecord{us}});
    const ParsedJournalFile seg = parse_segment_ok(segment);

    const BindingCheck ok = verify_segment_digest(seg, to_bytes(forward));
    CHECK(ok.ok);

    const BindingCheck bad =
        verify_segment_digest(seg, to_bytes(forward + "x"));
    CHECK_FALSE(bad.ok);
    CHECK_MESSAGE(bad.error.find("fwd_journal_digest") != std::string::npos, bad.error);
}

TEST_CASE("T-12 (segment): a forward journal handed to the segment classifier refuses") {
    const BuiltChain forward = build_chain({JournalRecord{make_run_start(1, false)}}, 0);
    const SegmentClassification c = classify_segment(forward.parsed);
    CHECK(c.state == SegmentState::Blocked);
    CHECK(c.reason == SegmentBlockReason::ParseInvalid);
}
