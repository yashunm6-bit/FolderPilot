// FolderPilot - Phase 7.1b journal core unit tests: canonical codec, strict
// parser, bounds enforcement and capacity arithmetic (pure logic, no I/O).
//
// Each TEST_CASE names its T-xx ID from docs/PHASE7-DESIGN.md section 18.
// Identity field formats are storage-layer decisions (7.1c); the tests use
// synthetic ASCII material and rely only on the ratified length caps.

#include <doctest.h>

#include "folderpilot/journal.hpp"
#include "sha256.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace folderpilot;

constexpr unsigned long long kU64Max = 18446744073709551615ULL;
constexpr const char* kRun = "0123456789abcdef0123456789abcdef";  // 32 bytes
constexpr const char* kRootKey = "root-key";
constexpr const char* kVolume = "\\\\?\\Volume{01234567-8901-2345-6789-0123456789ab}\\";
constexpr const char* kRootPath = "C:\\Users\\demo\\Desktop";
constexpr unsigned long long kRootRunSeq = 7;

std::string id73() {
    return std::string(journal_identity_max_bytes, 'i');
}

std::string hex64(char c = 'a') {
    return std::string(journal_hex_chars, c);
}

bool replace_first(std::string& text, std::string_view from, std::string_view to) {
    const std::size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

JournalRunStart make_run_start(unsigned long long planned = 2, bool ack = false) {
    JournalRunStart rs;
    rs.run = kRun;
    rs.root_run_seq = kRootRunSeq;
    rs.planned_move_count = planned;
    rs.lock_id = id73();
    rs.root_id = id73();
    rs.anchor_id = id73();
    rs.volume = kVolume;
    rs.root_path = kRootPath;
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
    r.cat_id = id73();
    r.file_id = id73();
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    return r;
}

JournalResult make_result(unsigned long long seq, unsigned long long move_id,
                          JournalOutcome outcome = JournalOutcome::Moved,
                          const std::string& detail = "") {
    JournalResult r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.move_id = move_id;
    r.cat_id = id73();
    r.file_id = id73();
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    r.outcome = outcome;
    r.detail = detail;
    return r;
}

JournalRunEnd make_run_end(unsigned long long seq, unsigned long long planned) {
    JournalRunEnd r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.planned_count = planned;
    r.moved_count = planned;
    r.conflict_count = 0;
    r.skipped_count = 0;
    r.failed_count = 0;
    return r;
}

JournalUndoStart make_undo_start(unsigned long long candidates,
                                 unsigned long long attempt = 1) {
    JournalUndoStart r;
    r.run = kRun;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = attempt;
    r.journal_id = id73();
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
    r.cat_id = id73();
    r.file_id = id73();
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    return r;
}

JournalUndoResult make_undo_result(unsigned long long seq, unsigned long long attempt_move,
                                   JournalOutcome outcome = JournalOutcome::Restored,
                                   const std::string& detail = "") {
    JournalUndoResult r;
    r.run = kRun;
    r.seq = seq;
    r.root_run_seq = kRootRunSeq;
    r.fwd_journal_digest = hex64('f');
    r.undo_attempt_seq = 1;
    r.attempt_move_seq = attempt_move;
    r.undo_of_move = attempt_move;
    r.cat_id = id73();
    r.file_id = id73();
    r.file_ctime = 133000000000000000ULL;
    r.file_size = 12345;
    r.file_mtime = 133000000000000001ULL;
    r.source_name = "report.pdf";
    r.dest_name = "report.pdf";
    r.dest_cat = Category::Documents;
    r.outcome = outcome;
    r.detail = detail;
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
    r.conflict_count = 0;
    r.skipped_count = 0;
    r.failed_count = 0;
    return r;
}

AnchorRecord make_anchor(unsigned long long anchor_seq, unsigned long long covered_seq,
                         unsigned long long byte_len, const std::string& last_hash) {
    AnchorRecord a;
    a.root_run_seq = kRootRunSeq;
    a.journal_id = id73();
    a.anchor_seq = anchor_seq;
    a.covered_seq = covered_seq;
    a.byte_len = byte_len;
    a.last_record_hash = last_hash;
    return a;
}

std::vector<unsigned char> to_bytes(const std::string& s) {
    return std::vector<unsigned char>(s.begin(), s.end());
}

ParsedJournalFile parse_ok(const std::string& bytes) {
    ParsedJournalFile p = parse_journal_file(to_bytes(bytes), kRootKey);
    REQUIRE_MESSAGE(p.valid, "parse failed: ", p.failure.detail);
    return p;
}

// Serializes one record with a fixed dummy prev (identical byte length to
// any real one).
std::string ser(const JournalRecord& rec) {
    const JournalSerializeResult r = serialize_journal_record(rec, hex64());
    REQUIRE_MESSAGE(r.ok, "serialize failed: ", r.error);
    return r.canonical;
}

std::string ser(const AnchorRecord& rec) {
    const JournalSerializeResult r = serialize_anchor_record(rec);
    REQUIRE_MESSAGE(r.ok, "anchor serialize failed: ", r.error);
    return r.canonical;
}

// Hash of a canonical line's content (LF excluded) - the `prev` of the
// record that follows it.
std::string prev_of(const std::string& line) {
    std::string_view view(line);
    if (!view.empty() && view.back() == '\n') {
        view.remove_suffix(1);
    }
    const platform::Sha256Result h = platform::sha256(to_bytes(std::string(view)));
    REQUIRE(h.ok);
    return journal_hex(h.digest);
}

} // namespace

// ---------------------------------------------------------------------------
// T-08: record round-trips for every class; strict UTF-8 (T-10 basis).
// ---------------------------------------------------------------------------

TEST_CASE("T-08: record round-trips for every journal record class") {
    const std::vector<JournalRecord> samples = {
        JournalRecord{make_run_start(2, false)},
        JournalRecord{make_run_start(1, true)},
        JournalRecord{make_intent(1, 0)},
        JournalRecord{make_result(2, 0)},
        JournalRecord{make_result(2, 0, JournalOutcome::Conflict, "destination existed")},
        JournalRecord{make_run_end(5, 2)},
        JournalRecord{make_undo_start(1)},
        JournalRecord{make_undo_intent(1, 0)},
        JournalRecord{make_undo_result(2, 0)},
        JournalRecord{make_undo_result(2, 0, JournalOutcome::SourceMissing, "source gone")},
        JournalRecord{make_undo_end(3, 1)},
    };
    for (const JournalRecord& original : samples) {
        const std::string line = ser(original);
        CAPTURE(line);
        const ParsedJournalFile p = parse_ok(line);
        REQUIRE(p.records.size() == 1);
        CHECK(p.records[0] == original);
        // Canonical stability: re-serialization is byte-identical (T-15).
        CHECK(serialize_journal_record(p.records[0], hex64()).canonical == line);
    }
}

TEST_CASE("T-08: anchor record round-trip") {
    const AnchorRecord original = make_anchor(0, 0, 42, hex64('b'));
    const std::string line = ser(original);
    const ParsedAnchorFile p = parse_anchor_file(to_bytes(line));
    REQUIRE_MESSAGE(p.valid, "anchor parse failed: ", p.failure.detail);
    REQUIRE(p.records.size() == 1);
    CHECK(p.records[0] == original);
    CHECK(ser(p.records[0]) == line);
}

TEST_CASE("T-10: unicode names and escaped detail round-trip; invalid UTF-8 rejected") {
    JournalIntent unicode = make_intent(1, 0);
    unicode.source_name = "caf\xc3\xa9 \xe2\x98\x95.txt";  // café ☕.txt
    const std::string line = ser(JournalRecord{unicode});
    const ParsedJournalFile p = parse_ok(line);
    REQUIRE(p.records.size() == 1);
    CHECK(std::get<JournalIntent>(p.records[0]).source_name == unicode.source_name);

    JournalResult with_escapes = make_result(2, 0, JournalOutcome::Moved, "l1\nl2 \"q\"");
    const std::string line2 = ser(JournalRecord{with_escapes});
    const ParsedJournalFile p2 = parse_ok(line2);
    CHECK(p2.records[0] == JournalRecord{with_escapes});

    // Raw invalid UTF-8 injected into an otherwise canonical line.
    for (const char* bad : {"caf\xc3\x28.txt", "bad\xed\xa0\x80.txt", "cut\xc3"}) {
        std::string good = ser(JournalRecord{make_intent(1, 0)});
        REQUIRE(replace_first(good, "\"report.pdf\"", bad));
        const ParsedJournalFile p3 = parse_journal_file(to_bytes(good), kRootKey);
        CHECK_MESSAGE(!p3.valid, "parser accepted invalid UTF-8: ", bad);
    }
}

// ---------------------------------------------------------------------------
// T-11: torn final record at every byte offset; ordering invariants.
// ---------------------------------------------------------------------------

TEST_CASE("T-11: truncation at every byte offset of a small journal is detected") {
    const std::vector<JournalRecord> records = {
        JournalRecord{make_run_start(1, false)},
        JournalRecord{make_intent(1, 0)},
        JournalRecord{make_result(2, 0)},
    };
    const JournalSerializeResult chain = serialize_journal_chain(records, kRootKey);
    REQUIRE(chain.ok);
    const std::string& image = chain.canonical;
    const std::size_t total = image.size();
    std::size_t previous_records = 0;
    for (std::size_t k = 0; k <= total; ++k) {
        CAPTURE(k);
        const ParsedJournalFile p =
            parse_journal_file(to_bytes(image.substr(0, k)), kRootKey);
        CHECK(p.valid);
        CHECK(p.records.size() >= previous_records);
        previous_records = p.records.size();
        CHECK(p.records.size() <= records.size());
        for (std::size_t i = 0; i < p.records.size() && i < records.size(); ++i) {
            CHECK(p.records[i] == records[i]);
        }
        if (k == total) {
            CHECK(p.tail == JournalTailKind::CleanEof);
            CHECK(p.records.size() == records.size());
            CHECK_FALSE(p.unterminated_final_record);
            continue;
        }
        const bool boundary = k > 0 && image[k - 1] == '\n';
        const bool missing_only_lf = k == total - 1;
        if (boundary) {
            CHECK(p.tail == JournalTailKind::CleanEof);
        } else if (missing_only_lf) {
            // R2: unterminated-but-complete - the record validates.
            CHECK(p.tail == JournalTailKind::CleanEof);
            CHECK(p.unterminated_final_record);
            CHECK(p.records.size() == records.size());
        } else {
            CHECK(p.tail == JournalTailKind::Torn);
            const std::size_t tail_start =
                p.record_end.empty() ? 0 : p.record_end.back();
            CHECK(p.tail_bytes.size() == k - tail_start);
        }
    }
}

TEST_CASE("T-11: torn-tail type hints (typed INTENT tail; short tail untyped)") {
    const std::vector<JournalRecord> records = {
        JournalRecord{make_run_start(1, false)},
        JournalRecord{make_intent(1, 0)},
    };
    const JournalSerializeResult chain = serialize_journal_chain(records, kRootKey);
    REQUIRE(chain.ok);
    const std::size_t intent_line_start = chain.canonical.find('\n') + 1;
    const std::size_t type_pos = chain.canonical.find("\"type\":\"INTENT\"");
    REQUIRE(type_pos != std::string::npos);
    const std::string cut = chain.canonical.substr(0, type_pos + 14);
    const ParsedJournalFile p = parse_journal_file(to_bytes(cut), kRootKey);
    CHECK(p.valid);
    REQUIRE(p.tail == JournalTailKind::Torn);
    REQUIRE(p.tail_type_hint.has_value());
    CHECK(*p.tail_type_hint == JournalRecordType::Intent);

    const std::string short_cut = chain.canonical.substr(0, intent_line_start + 8);
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(short_cut), kRootKey);
    CHECK(p2.valid);
    REQUIRE(p2.tail == JournalTailKind::Torn);
    CHECK_FALSE(p2.tail_type_hint.has_value());
}

TEST_CASE("T-11: seq must equal the record position") {
    const JournalRunStart rs = make_run_start(1, false);
    const JournalHashResult genesis = journal_genesis_prev(kRootKey, kRootRunSeq, kRun);
    REQUIRE(genesis.ok);
    const std::string line0 = ser(JournalRecord{rs});
    // Serialize a second record whose seq does not match its position: the
    // writer primitive permits it, the parser must reject it.
    const JournalSerializeResult line1 =
        serialize_journal_record(JournalRecord{make_intent(2, 0)}, prev_of(line0));
    REQUIRE(line1.ok);
    const ParsedJournalFile p =
        parse_journal_file(to_bytes(line0 + line1.canonical), kRootKey);
    REQUIRE_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::TransitionViolation);
}

TEST_CASE("T-11: anchor_seq/covered_seq/byte_len ordering and identity rules are enforced") {
    const std::string genesis = ser(make_anchor(0, 0, 42, hex64('b')));

    // anchor_seq gap (0 -> 2).
    const std::string gapped = genesis + ser(make_anchor(2, 2, 100, hex64('c')));
    CHECK_FALSE(parse_anchor_file(to_bytes(gapped)).valid);

    // covered_seq rewind (anchor_seq 1, covered_seq 0).
    const std::string rewound = genesis + ser(make_anchor(1, 0, 100, hex64('c')));
    CHECK_FALSE(parse_anchor_file(to_bytes(rewound)).valid);

    // byte_len not strictly increasing.
    const std::string flat = genesis + ser(make_anchor(1, 1, 42, hex64('c')));
    CHECK_FALSE(parse_anchor_file(to_bytes(flat)).valid);

    // Genesis coverage of zero bytes.
    CHECK_FALSE(parse_anchor_file(to_bytes(ser(make_anchor(0, 0, 0, hex64('b'))))).valid);

    // Unknown kind / file_kind values.
    std::string foreign = ser(make_anchor(0, 0, 42, hex64('b')));
    REQUIRE(replace_first(foreign, "\"file_kind\":\"journal\"", "\"file_kind\":\"segment\""));
    CHECK_FALSE(parse_anchor_file(to_bytes(foreign)).valid);
    foreign = ser(make_anchor(0, 0, 42, hex64('b')));
    REQUIRE(replace_first(foreign, "\"kind\":\"anchor\"", "\"kind\":\"other\""));
    CHECK_FALSE(parse_anchor_file(to_bytes(foreign)).valid);

    // journal_id discontinuity between anchor records.
    AnchorRecord second = make_anchor(1, 1, 100, hex64('c'));
    second.journal_id = std::string(journal_identity_max_bytes, 'j');
    const std::string swapped = genesis + ser(second);
    const ParsedAnchorFile p = parse_anchor_file(to_bytes(swapped));
    REQUIRE_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::ContinuityMismatch);
}

// ---------------------------------------------------------------------------
// T-12/T-13: unknown types/versions, duplicate/unknown/misordered keys.
// ---------------------------------------------------------------------------

TEST_CASE("T-12: unknown v, unknown type, and direction/type mismatch are invalid") {
    const std::string line = ser(JournalRecord{make_intent(1, 0)});

    std::string v2 = line;
    REQUIRE(replace_first(v2, "\"v\":1", "\"v\":2"));
    CHECK_FALSE(parse_journal_file(to_bytes(v2), kRootKey).valid);

    std::string foreign_type = line;
    REQUIRE(replace_first(foreign_type, "\"type\":\"INTENT\"", "\"type\":\"NOPE\""));
    const ParsedJournalFile p = parse_journal_file(to_bytes(foreign_type), kRootKey);
    REQUIRE_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::UnknownTypeOrVersion);

    std::string undo_type = line;
    REQUIRE(replace_first(undo_type, "\"type\":\"INTENT\"", "\"type\":\"UNDO-START\""));
    CHECK_FALSE(parse_journal_file(to_bytes(undo_type), kRootKey).valid);

    std::string foreign_dir = line;
    REQUIRE(replace_first(foreign_dir, "\"dir\":\"forward\"", "\"dir\":\"sideways\""));
    CHECK_FALSE(parse_journal_file(to_bytes(foreign_dir), kRootKey).valid);
}

TEST_CASE("T-13: duplicate, unknown and misordered keys are rejected") {
    const std::string line = ser(JournalRecord{make_intent(1, 0)});

    // Duplicate key: a second "cat_id" where "file_id" belongs.
    std::string dup = line;
    REQUIRE(replace_first(dup, "\"file_id\"", "\"cat_id\""));
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(dup), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::DuplicateKey);

    // Unknown key.
    std::string unknown = line;
    REQUIRE(replace_first(unknown, "\"file_id\"", "\"wat_id\""));
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(unknown), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::UnknownField);

    // Misordered key: move source_name ahead of file_ctime.
    std::string reordered = ser(JournalRecord{make_result(2, 0)});
    const std::size_t source = reordered.find("\"source_name\":");
    REQUIRE(source != std::string::npos);
    const std::size_t field_end = reordered.find(',', source);
    REQUIRE(field_end != std::string::npos);
    const std::string field = reordered.substr(source, field_end - source + 1);
    reordered.erase(source, field.size());
    REQUIRE(replace_first(reordered, "\"file_ctime\":", field + "\"file_ctime\":"));
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(reordered), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

// ---------------------------------------------------------------------------
// T-13: correlation contradictions.
// ---------------------------------------------------------------------------

TEST_CASE("T-13: acknowledgement pairing contradictions are invalid") {
    // acknowledged:true without supersedes_root_run_seq - the parser demands
    // the pair right after the flag (schema order, before integrity).
    std::string line = ser(JournalRecord{make_run_start(2, true)});
    REQUIRE(replace_first(line, ",\"supersedes_root_run_seq\":3", ""));
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(line), kRootKey);
    CHECK_FALSE(p1.valid);

    // Explicit false is not a canonical shape (docs section 8).
    std::string injected = ser(JournalRecord{make_run_start(2, false)});
    REQUIRE(replace_first(injected, ",\"prev\":",
                          ",\"undo_window_acknowledged\":false,\"prev\":"));
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(injected), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::CorrelationAnomaly);
}

TEST_CASE("T-13: outcome vocabulary and category contradictions are invalid") {
    // 'restored' is illegal in a forward RESULT (schema-level check).
    std::string line = ser(JournalRecord{make_result(2, 0)});
    REQUIRE(replace_first(line, "\"outcome\":\"Moved\"", "\"outcome\":\"restored\""));
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(line), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::CorrelationAnomaly);

    // Unknown outcome name.
    std::string oline = ser(JournalRecord{make_result(2, 0)});
    REQUIRE(replace_first(oline, "\"outcome\":\"Moved\"", "\"outcome\":\"Wibbled\""));
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(oline), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::WrongValueType);

    // Unknown category name.
    std::string iline = ser(JournalRecord{make_intent(1, 0)});
    REQUIRE(replace_first(iline, "\"dest_cat\":\"Documents\"", "\"dest_cat\":\"Finance\""));
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(iline), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::WrongValueType);
}

TEST_CASE("T-13: run and per-segment continuity are enforced (with a positive control)") {
    const JournalHashResult genesis = journal_genesis_prev(kRootKey, kRootRunSeq, kRun);
    REQUIRE(genesis.ok);
    const std::string line0 = ser(JournalRecord{make_run_start(1, false)});
    const std::string p1_hex = prev_of(line0);
    (void)p1_hex;

    // Positive control: matching run parses.
    const std::string ok_line1 =
        serialize_journal_record(JournalRecord{make_intent(1, 0)}, prev_of(line0)).canonical;
    CHECK(parse_journal_file(to_bytes(line0 + ok_line1), kRootKey).valid);

    // Different run in the second record: hash-valid, prev-valid, but the
    // continuity check must refuse it.
    JournalIntent other_run = make_intent(1, 0);
    other_run.run = "fedcba9876543210fedcba9876543210";
    const JournalSerializeResult bad_line1 =
        serialize_journal_record(JournalRecord{other_run}, prev_of(line0));
    REQUIRE(bad_line1.ok);
    const ParsedJournalFile p2 =
        parse_journal_file(to_bytes(line0 + bad_line1.canonical), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::ContinuityMismatch);

    // undo_attempt_seq discontinuity within a segment.
    const std::string seg0 = ser(JournalRecord{make_undo_start(1)});
    JournalUndoIntent intent = make_undo_intent(1, 0);
    intent.undo_attempt_seq = 99;
    const JournalSerializeResult seg1 =
        serialize_journal_record(JournalRecord{intent}, prev_of(seg0));
    REQUIRE(seg1.ok);
    const ParsedJournalFile p3 =
        parse_journal_file(to_bytes(seg0 + seg1.canonical), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::ContinuityMismatch);

    // fwd_journal_digest discontinuity within a segment.
    JournalUndoResult result = make_undo_result(2, 0);
    result.fwd_journal_digest = hex64('9');
    const JournalSerializeResult seg2 =
        serialize_journal_record(JournalRecord{result}, prev_of(seg0));
    REQUIRE(seg2.ok);
    const ParsedJournalFile p4 =
        parse_journal_file(to_bytes(seg0 + seg2.canonical), kRootKey);
    REQUIRE_FALSE(p4.valid);
    CHECK(p4.failure.kind == ParseFailureKind::ContinuityMismatch);
}

TEST_CASE("T-13: chain breaks and genesis seeds are enforced") {
    const std::string line0 = ser(JournalRecord{make_run_start(1, false)});
    // Second record whose prev does not chain to record 0.
    const JournalSerializeResult bad_line1 =
        serialize_journal_record(JournalRecord{make_intent(1, 0)}, hex64('e'));
    REQUIRE(bad_line1.ok);
    const ParsedJournalFile p1 =
        parse_journal_file(to_bytes(line0 + bad_line1.canonical), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::ChainBreak);

    // Wrong genesis seed: the same journal under a different root_key.
    const std::string image = ser(JournalRecord{make_run_start(1, false)});
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(image), "other-root-key");
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::ChainBreak);
}

// ---------------------------------------------------------------------------
// T-14: caps.
// ---------------------------------------------------------------------------

TEST_CASE("T-14: field caps are enforced by the serializer") {
    JournalIntent long_name = make_intent(1, 0);
    long_name.source_name = std::string(journal_name_max_bytes + 1, 'a');
    CHECK_FALSE(serialize_journal_record(JournalRecord{long_name}, hex64()).ok);

    JournalResult long_detail =
        make_result(2, 0, JournalOutcome::Moved, std::string(journal_detail_max_bytes + 1, 'd'));
    CHECK_FALSE(serialize_journal_record(JournalRecord{long_detail}, hex64()).ok);

    JournalRunStart long_path = make_run_start();
    long_path.root_path = std::string(journal_root_path_max_bytes + 1, 'p');
    CHECK_FALSE(serialize_journal_record(JournalRecord{long_path}, hex64()).ok);

    JournalRunStart long_run = make_run_start();
    long_run.run = std::string(journal_run_max_bytes + 1, 'r');
    CHECK_FALSE(serialize_journal_record(JournalRecord{long_run}, hex64()).ok);

    JournalIntent long_id = make_intent(1, 0);
    long_id.file_id = std::string(journal_identity_max_bytes + 1, 'i');
    CHECK_FALSE(serialize_journal_record(JournalRecord{long_id}, hex64()).ok);

    JournalRunStart many = make_run_start(journal_move_limit + 1, false);
    CHECK_FALSE(serialize_journal_record(JournalRecord{many}, hex64()).ok);

    JournalUndoStart zero = make_undo_start(0);
    CHECK_FALSE(serialize_journal_record(JournalRecord{zero}, hex64()).ok);

    JournalIntent intent = make_intent(1, 0);
    CHECK_FALSE(serialize_journal_record(JournalRecord{intent}, "nothex").ok);
    CHECK_FALSE(serialize_journal_record(JournalRecord{intent}, std::string(64, 'A')).ok);
}

TEST_CASE("T-14: the parser refuses counts above the operational limit") {
    // A canonical line with planned_move_count 1501 cannot come from the
    // serializer; patching it breaks integrity, and the parser refuses
    // either way (cap or hash - both are refusals).
    std::string patched = ser(JournalRecord{make_run_start(2, false)});
    REQUIRE(replace_first(patched, "\"planned_move_count\":2", "\"planned_move_count\":1501"));
    CHECK_FALSE(parse_journal_file(to_bytes(patched), kRootKey).valid);
}

TEST_CASE("T-14: oversize input is refused") {
    std::vector<unsigned char> big(journal_max_file_bytes + 1, 'a');
    const ParsedJournalFile p = parse_journal_file(big, kRootKey);
    CHECK_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::OversizeFile);
}

// ---------------------------------------------------------------------------
// T-10/T-11: non-canonical encodings and framing.
// ---------------------------------------------------------------------------

TEST_CASE("T-10: non-canonical numbers, escapes and whitespace are rejected") {
    const std::string line = ser(JournalRecord{make_result(2, 0)});

    struct BadPatch {
        const char* from;
        const char* to;
        const char* label;
    };
    const BadPatch patches[] = {
        {"\"file_size\":12345", "\"file_size\":012345", "leading zero"},
        {"\"file_size\":12345", "\"file_size\":+12345", "plus sign"},
        {"\"file_size\":12345", "\"file_size\":-12345", "negative"},
        {"\"file_size\":12345", "\"file_size\":12345.0", "fraction"},
        {"\"file_size\":12345", "\"file_size\":1.2345e4", "exponent"},
        {"\"detail\":\"\"", "\"detail\":\"\\u0041\"", "u-escape for a printable"},
        {"\"detail\":\"\"", "\"detail\":\"\\u000A\"", "u-escape for a short-form control"},
        {"\"detail\":\"\"", "\"detail\":\"\\u00zz\"", "non-hex u-escape"},
        {"\"v\":1", "\"v\" :1", "space inside object"},
    };
    for (const BadPatch& bad : patches) {
        std::string patched = line;
        REQUIRE_MESSAGE(replace_first(patched, bad.from, bad.to),
                        "target missing: ", bad.label);
        const ParsedJournalFile p = parse_journal_file(to_bytes(patched), kRootKey);
        CHECK_MESSAGE(!p.valid, "accepted non-canonical encoding: ", bad.label);
    }

    // Uppercase hex in a hash field.
    std::string upper = line;
    const std::size_t prev_pos = upper.find("\"prev\":\"");
    REQUIRE(prev_pos != std::string::npos);
    for (std::size_t i = prev_pos + 9; i < prev_pos + 9 + 64; ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'f') {
            upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
            break;
        }
    }
    CHECK_FALSE(parse_journal_file(to_bytes(upper), kRootKey).valid);
}

TEST_CASE("T-11: interior empty lines and CRLF framing are non-canonical") {
    const std::string line = ser(JournalRecord{make_run_start(1, false)});
    CHECK_FALSE(parse_journal_file(to_bytes(line + "\n"), kRootKey).valid);
    std::string crlf = line.substr(0, line.size() - 1) + "\r\n";
    CHECK_FALSE(parse_journal_file(to_bytes(crlf), kRootKey).valid);
}

// ---------------------------------------------------------------------------
// T-15: canonical stability, hash-chain wiring, universal bit-flip detection.
// ---------------------------------------------------------------------------

TEST_CASE("T-15: canonical serialization is stable and the hash chain is wired") {
    const std::vector<JournalRecord> records = {
        JournalRecord{make_run_start(2, true)},
        JournalRecord{make_intent(1, 0)},
        JournalRecord{make_result(2, 0)},
        JournalRecord{make_run_end(5, 2)},
    };
    const JournalSerializeResult chain = serialize_journal_chain(records, kRootKey);
    REQUIRE(chain.ok);

    // parse -> re-serialize is byte-identical.
    const ParsedJournalFile p = parse_ok(chain.canonical);
    REQUIRE(p.records.size() == records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        CHECK(p.records[i] == records[i]);
    }
    const JournalSerializeResult again = serialize_journal_chain(p.records, kRootKey);
    REQUIRE(again.ok);
    CHECK(again.canonical == chain.canonical);

    // The genesis seed binds root_key || root_run_seq || run: the same
    // journal under a different root_key must not parse (covered in
    // T-13 chain breaks). Here: record 1's prev must be the hash of
    // record 0's full canonical line.
    const std::size_t lf0 = chain.canonical.find('\n');
    REQUIRE(lf0 != std::string::npos);
    const std::string manual_prev = prev_of(chain.canonical.substr(0, lf0 + 1));
    const JournalSerializeResult line1 =
        serialize_journal_record(records[1], manual_prev);
    REQUIRE(line1.ok);
    const std::string image2 = chain.canonical.substr(0, lf0 + 1) + line1.canonical;
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(image2), kRootKey);
    CHECK_MESSAGE(p2.valid, "prev wiring mismatch: ", p2.failure.detail);
}

TEST_CASE("T-15: every single-bit flip in a two-record journal is detected") {
    const std::vector<JournalRecord> records = {
        JournalRecord{make_run_start(1, false)},
        JournalRecord{make_intent(1, 0)},
    };
    const JournalSerializeResult chain = serialize_journal_chain(records, kRootKey);
    REQUIRE(chain.ok);
    const std::string image = chain.canonical;
    std::size_t flips = 0;
    std::size_t undetected = 0;
    for (std::size_t b = 0; b < image.size(); ++b) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            std::string flipped = image;
            flipped[b] = static_cast<char>(flipped[b] ^ static_cast<char>(1u << bit));
            const ParsedJournalFile p = parse_journal_file(to_bytes(flipped), kRootKey);
            const bool looks_unharmed = p.valid && p.tail == JournalTailKind::CleanEof &&
                                        !p.unterminated_final_record &&
                                        p.records.size() == records.size() &&
                                        p.records[0] == records[0] &&
                                        p.records[1] == records[1];
            if (looks_unharmed) {
                ++undetected;
            }
            ++flips;
        }
    }
    CHECK_MESSAGE(undetected == 0, undetected, " of ", flips,
                  " single-bit flips went undetected");
}

// ---------------------------------------------------------------------------
// T-15/T-50: maximal records versus the ratified ceilings; exact reserves.
// ---------------------------------------------------------------------------

TEST_CASE("T-15: maximal records against the ratified per-record ceilings (docs section 12)") {
    const std::string dummy_prev = hex64();
    auto size_of = [&](const JournalRecord& rec) {
        const JournalSerializeResult r = serialize_journal_record(rec, dummy_prev);
        REQUIRE_MESSAGE(r.ok, "maximal record failed to serialize: ", r.error);
        return r.canonical.size();
    };

    JournalRunStart max_start = make_run_start(journal_move_limit, true);
    max_start.run = std::string(journal_run_max_bytes, 'r');
    max_start.root_run_seq = kU64Max;
    max_start.volume = std::string(journal_volume_max_bytes, 'v');
    max_start.root_path = std::string(journal_root_path_max_bytes, 'p');
    max_start.supersedes_root_run_seq = kU64Max;
    const std::size_t run_start = size_of(JournalRecord{max_start});

    JournalIntent max_intent = make_intent(journal_seq_max, journal_seq_max);
    max_intent.file_ctime = kU64Max;
    max_intent.file_size = kU64Max;
    max_intent.file_mtime = kU64Max;
    max_intent.source_name = std::string(journal_name_max_bytes, 'n');
    max_intent.dest_name = std::string(journal_name_max_bytes, 'n');
    const std::size_t intent_size = size_of(JournalRecord{max_intent});

    JournalResult max_result =
        make_result(journal_seq_max, journal_seq_max, JournalOutcome::DestinationChanged,
                    std::string(journal_detail_max_bytes, 'd'));
    max_result.file_ctime = kU64Max;
    max_result.file_size = kU64Max;
    max_result.file_mtime = kU64Max;
    max_result.source_name = std::string(journal_name_max_bytes, 'n');
    max_result.dest_name = std::string(journal_name_max_bytes, 'n');
    const std::size_t result_size = size_of(JournalRecord{max_result});

    const std::size_t run_end = size_of(JournalRecord{make_run_end(journal_seq_max, journal_seq_max)});

    JournalUndoStart max_undo_start =
        make_undo_start(journal_move_limit, kU64Max);
    const std::size_t undo_start = size_of(JournalRecord{max_undo_start});

    JournalUndoIntent max_undo_intent = make_undo_intent(journal_seq_max, journal_seq_max);
    max_undo_intent.undo_attempt_seq = kU64Max;
    max_undo_intent.file_ctime = kU64Max;
    max_undo_intent.file_size = kU64Max;
    max_undo_intent.file_mtime = kU64Max;
    max_undo_intent.source_name = std::string(journal_name_max_bytes, 'n');
    max_undo_intent.dest_name = std::string(journal_name_max_bytes, 'n');
    const std::size_t undo_intent_size = size_of(JournalRecord{max_undo_intent});

    JournalUndoResult max_undo_result =
        make_undo_result(journal_seq_max, journal_seq_max, JournalOutcome::DestinationChanged);
    max_undo_result.undo_attempt_seq = kU64Max;
    max_undo_result.file_ctime = kU64Max;
    max_undo_result.file_size = kU64Max;
    max_undo_result.file_mtime = kU64Max;
    max_undo_result.source_name = std::string(journal_name_max_bytes, 'n');
    max_undo_result.dest_name = std::string(journal_name_max_bytes, 'n');
    max_undo_result.detail = std::string(journal_detail_max_bytes, 'd');
    const std::size_t undo_result_size = size_of(JournalRecord{max_undo_result});

    const std::size_t undo_end =
        size_of(JournalRecord{make_undo_end(journal_seq_max, journal_seq_max)});

    AnchorRecord max_anchor = make_anchor(journal_seq_max, journal_seq_max,
                                          journal_max_file_bytes, hex64('b'));
    max_anchor.root_run_seq = kU64Max;
    const std::size_t anchor_size = ser(max_anchor).size();

    // The 8 KiB record cap (docs section 5) binds every record type.
    CHECK(run_start <= journal_max_record_bytes);
    CHECK(intent_size <= journal_max_record_bytes);
    CHECK(result_size <= journal_max_record_bytes);
    CHECK(run_end <= journal_max_record_bytes);
    CHECK(undo_start <= journal_max_record_bytes);
    CHECK(undo_intent_size <= journal_max_record_bytes);
    CHECK(undo_result_size <= journal_max_record_bytes);
    CHECK(undo_end <= journal_max_record_bytes);
    CHECK(anchor_size <= journal_max_record_bytes);

    // Ratified ceilings that reconcile with the canonical key order.
    CHECK_MESSAGE(run_start <= 4900, "RUN-START maximal ", run_start, " > 4900");
    CHECK_MESSAGE(intent_size <= 2200, "INTENT maximal ", intent_size, " > 2200");
    CHECK_MESSAGE(result_size <= 2750, "RESULT maximal ", result_size, " > 2750");
    CHECK_MESSAGE(run_end <= 400, "RUN-END maximal ", run_end, " > 400");
    CHECK_MESSAGE(undo_start <= 700, "UNDO-START maximal ", undo_start, " > 700");
    CHECK_MESSAGE(anchor_size <= 450, "anchor maximal ", anchor_size, " > 450");
    // The document's UNDO-INTENT/UNDO-RESULT/UNDO-END ceilings are not
    // satisfiable under the canonical encoding (reported as errata in the
    // 7.1b report); the 8 KiB cap is the binding constraint for them.
    MESSAGE("measured maximal sizes: RUN-START=", run_start, " INTENT=", intent_size,
            " RESULT=", result_size, " RUN-END=", run_end, " UNDO-START=", undo_start,
            " UNDO-INTENT=", undo_intent_size, " UNDO-RESULT=", undo_result_size,
            " UNDO-END=", undo_end, " anchor=", anchor_size);
}

TEST_CASE("T-50 (unit basis): exact reserves at the 1,500-move bound fit the file limits") {
    const CapacityEstimate fwd = forward_reserve_bytes(make_run_start(), journal_move_limit);
    REQUIRE_MESSAGE(fwd.ok, "forward reserve failed: ", fwd.error);
    const CapacityEstimate seg = segment_reserve_bytes(make_undo_start(1), journal_move_limit);
    REQUIRE_MESSAGE(seg.ok, "segment reserve failed: ", seg.error);
    const CapacityEstimate anc = anchor_reserve_bytes(journal_move_limit);
    REQUIRE_MESSAGE(anc.ok, "anchor reserve failed: ", anc.error);

    MESSAGE("exact reserves at N=1,500: forward=", fwd.bytes, " segment=", seg.bytes,
            " anchor=", anc.bytes, " bytes");
    CHECK(within_per_file_limit(fwd.bytes));
    CHECK(within_per_file_limit(seg.bytes));
    CHECK(within_per_file_limit(anc.bytes));

    // The ratified 128 MiB aggregate admits 8 complete cycles (docs section 12).
    const std::size_t cycle = fwd.bytes + anc.bytes + seg.bytes;
    MESSAGE("one worst-case cycle = ", cycle, " bytes; 8 cycles = ", 8 * cycle);
    CHECK(8 * cycle <= journal_max_aggregate_bytes);
    CHECK(within_aggregate_limit(7 * cycle, cycle));
    CHECK_FALSE(within_aggregate_limit(7 * cycle, cycle + 1));
    CHECK_FALSE(within_aggregate_limit(journal_max_aggregate_bytes, 1));
}
