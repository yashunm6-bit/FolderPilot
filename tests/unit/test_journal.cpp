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
#include <variant>
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
    // The pair serializes only with has_ack=true; an ack-less RUN-START
    // parses with the payload default (0), so the fixture must match.
    rs.supersedes_root_run_seq = ack ? 3 : 0;
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
std::string chain_image(const std::vector<JournalRecord>& records) {
    const JournalSerializeResult r = serialize_journal_chain(records, kRootKey);
    REQUIRE_MESSAGE(r.ok, "chain serialize failed: ", r.error);
    return r.canonical;
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

// Replaces the first `from`->`to` occurrence at or after `begin`; fails the
// test when the target is missing (or would patch an earlier line).
bool patch_from(std::string& text, std::size_t begin, std::string_view from,
                std::string_view to) {
    const std::size_t pos = text.find(from, begin);
    if (pos == std::string::npos) {
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

// Minimal valid record sequence containing `rec` at the position its `seq`
// names: the parser's expected-record machine requires a START head and
// gap-free per-move INTENT/RESULT pairing, so a bare later-record line is
// never a parseable file by itself.
std::vector<JournalRecord> file_containing(const JournalRecord& rec) {
    switch (rec.index()) {
        case 0:  // RUN-START
            return {rec};
        case 1:  // INTENT
            return {JournalRecord{make_run_start(1, false)}, rec};
        case 2:  // RESULT
            return {JournalRecord{make_run_start(1, false)},
                    JournalRecord{make_intent(1, 0)}, rec};
        case 3:  // RUN-END
            return {JournalRecord{make_run_start(2, false)},
                    JournalRecord{make_intent(1, 0)},
                    JournalRecord{make_result(2, 0)},
                    JournalRecord{make_intent(3, 1)},
                    JournalRecord{make_result(4, 1)}, rec};
        case 4:  // UNDO-START
            return {rec};
        case 5:  // UNDO-INTENT
            return {JournalRecord{make_undo_start(1)}, rec};
        case 6:  // UNDO-RESULT
            return {JournalRecord{make_undo_start(1)},
                    JournalRecord{make_undo_intent(1, 0)}, rec};
        default:  // UNDO-END
            return {JournalRecord{make_undo_start(1)},
                    JournalRecord{make_undo_intent(1, 0)},
                    JournalRecord{make_undo_result(2, 0)}, rec};
    }
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
        CAPTURE(original.index());
        const std::vector<JournalRecord> file = file_containing(original);
        const std::string image = chain_image(file);
        const ParsedJournalFile p = parse_ok(image);
        REQUIRE(p.records.size() == file.size());
        // The sample sits at the position its seq names (the last record).
        CHECK(p.records.back() == original);
        // Canonical stability: re-serializing the parsed records recomputes
        // the genesis seed and the prev chain from the same fields, so it
        // reuses the same prev values and is byte-identical (T-15).
        const JournalSerializeResult again = serialize_journal_chain(p.records, kRootKey);
        REQUIRE(again.ok);
        CHECK(again.canonical == image);
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
    const ParsedJournalFile p =
        parse_ok(chain_image({JournalRecord{make_run_start(1, false)},
                              JournalRecord{unicode}}));
    REQUIRE(p.records.size() == 2);
    CHECK(std::get<JournalIntent>(p.records[1]).source_name == unicode.source_name);

    JournalResult with_escapes = make_result(2, 0, JournalOutcome::Moved, "l1\nl2 \"q\"");
    const ParsedJournalFile p2 =
        parse_ok(chain_image({JournalRecord{make_run_start(1, false)},
                              JournalRecord{make_intent(1, 0)},
                              JournalRecord{with_escapes}}));
    REQUIRE(p2.records.size() == 3);
    CHECK(p2.records[2] == JournalRecord{with_escapes});

    // Raw invalid UTF-8 injected into an otherwise valid two-record chain
    // (inside the quotes, so the value still parses as a string and the
    // failure is the encoding itself, not a missing quote).
    for (const char* bad : {"caf\xc3\x28.txt", "bad\xed\xa0\x80.txt", "cut\xc3"}) {
        std::string good = chain_image({JournalRecord{make_run_start(1, false)},
                                        JournalRecord{make_intent(1, 0)}});
        const std::string injected = std::string("\"") + bad + "\"";
        REQUIRE_MESSAGE(patch_from(good, line_offset(good, 1), "\"report.pdf\"", injected),
                        "utf-8 patch target missing");
        const ParsedJournalFile p3 = parse_journal_file(to_bytes(good), kRootKey);
        CHECK_MESSAGE(!p3.valid, "parser accepted invalid UTF-8: ", bad);
        CHECK(p3.failure.kind == ParseFailureKind::InvalidUtf8);
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
        if (k == 0) {
            // The zero-byte prefix is the clean empty file: valid, no
            // records, no tail bytes, nothing unterminated.
            CHECK(p.tail == JournalTailKind::CleanEof);
            CHECK(p.records.empty());
            CHECK(p.tail_bytes.empty());
            CHECK_FALSE(p.unterminated_final_record);
            previous_records = 0;
            continue;
        }
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
            // The tail is either an R2 unterminated-but-complete record or
            // genuinely torn; both are valid. A `}` inside a value (e.g. the
            // volume GUID's closing brace) is not a record boundary, so the
            // outcome is decided by the parse itself.
            if (p.tail == JournalTailKind::CleanEof) {
                CHECK(p.unterminated_final_record);
                CHECK(p.tail_bytes.empty());
            } else {
                CHECK(p.tail == JournalTailKind::Torn);
                const std::size_t tail_start =
                    p.record_end.empty() ? 0 : p.record_end.back();
                CHECK(p.tail_bytes.size() == k - tail_start);
            }
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
    // Record 0 must carry the genesis seed for the parse to reach the
    // intended seq-position failure.
    const std::string line0 = ser_first(JournalRecord{rs});
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
    // A valid two-record chain; every patch lands on the INTENT line so the
    // intended check is what refuses (the head stays parseable).
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)}});
    const std::size_t intent_line = line_offset(image, 1);

    std::string v2 = image;
    REQUIRE_MESSAGE(patch_from(v2, intent_line, "\"v\":1", "\"v\":2"), "v patch missing");
    const ParsedJournalFile pv = parse_journal_file(to_bytes(v2), kRootKey);
    REQUIRE_FALSE(pv.valid);
    CHECK(pv.failure.kind == ParseFailureKind::UnknownTypeOrVersion);

    std::string foreign_type = image;
    REQUIRE_MESSAGE(patch_from(foreign_type, intent_line,
                               "\"type\":\"INTENT\"", "\"type\":\"NOPE\""),
                    "type patch missing");
    const ParsedJournalFile p = parse_journal_file(to_bytes(foreign_type), kRootKey);
    REQUIRE_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::UnknownTypeOrVersion);

    std::string undo_type = image;
    REQUIRE_MESSAGE(patch_from(undo_type, intent_line,
                               "\"type\":\"INTENT\"", "\"type\":\"UNDO-START\""),
                    "undo-type patch missing");
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(undo_type), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::TransitionViolation);

    std::string foreign_dir = image;
    REQUIRE_MESSAGE(patch_from(foreign_dir, intent_line,
                               "\"dir\":\"forward\"", "\"dir\":\"sideways\""),
                    "dir patch missing");
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(foreign_dir), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::UnknownTypeOrVersion);
}

TEST_CASE("T-13: duplicate, unknown and misordered keys are rejected") {
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)},
                                           JournalRecord{make_result(2, 0)}});
    const std::size_t intent_line = line_offset(image, 1);
    const std::size_t result_line = line_offset(image, 2);

    // Duplicate key: a second "cat_id" where "file_id" belongs (INTENT line).
    std::string dup = image;
    REQUIRE_MESSAGE(patch_from(dup, intent_line, "\"file_id\"", "\"cat_id\""),
                    "dup patch missing");
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(dup), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::DuplicateKey);

    // Unknown key (INTENT line).
    std::string unknown = image;
    REQUIRE_MESSAGE(patch_from(unknown, intent_line, "\"file_id\"", "\"wat_id\""),
                    "unknown patch missing");
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(unknown), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::UnknownField);

    // Misordered key: move source_name ahead of file_ctime (RESULT line).
    std::string reordered = image;
    const std::size_t source = reordered.find("\"source_name\":", result_line);
    REQUIRE(source != std::string::npos);
    const std::size_t field_end = reordered.find(',', source);
    REQUIRE(field_end != std::string::npos);
    const std::string field = reordered.substr(source, field_end - source + 1);
    reordered.erase(source, field.size());
    REQUIRE_MESSAGE(patch_from(reordered, result_line, "\"file_ctime\":",
                               field + "\"file_ctime\":"),
                    "reorder insert target missing");
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(reordered), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

// ---------------------------------------------------------------------------
// T-13: correlation contradictions.
// ---------------------------------------------------------------------------

TEST_CASE("T-13: acknowledgement pairing contradictions are invalid") {
    // acknowledged:true without supersedes_root_run_seq - the parser demands
    // the pair right after the flag (schema order, before integrity). The
    // head itself is valid (genesis prev), so the pairing is what fails.
    std::string line = ser_first(JournalRecord{make_run_start(2, true)});
    REQUIRE(replace_first(line, ",\"supersedes_root_run_seq\":3", ""));
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(line), kRootKey);
    CHECK_FALSE(p1.valid);

    // Explicit false is not a canonical shape (docs section 8), injected
    // into an otherwise valid RUN-START.
    std::string injected = ser_first(JournalRecord{make_run_start(2, false)});
    REQUIRE(replace_first(injected, ",\"prev\":",
                          ",\"undo_window_acknowledged\":false,\"prev\":"));
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(injected), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::CorrelationAnomaly);
}

TEST_CASE("T-13: outcome vocabulary and category contradictions are invalid") {
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)},
                                           JournalRecord{make_result(2, 0)}});
    const std::size_t result_line = line_offset(image, 2);
    const std::size_t intent_line = line_offset(image, 1);

    // 'restored' is illegal in a forward RESULT (schema-level check).
    std::string line = image;
    REQUIRE_MESSAGE(patch_from(line, result_line, "\"outcome\":\"Moved\"",
                               "\"outcome\":\"restored\""),
                    "outcome patch missing");
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(line), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::CorrelationAnomaly);

    // Unknown outcome name.
    std::string oline = image;
    REQUIRE_MESSAGE(patch_from(oline, result_line, "\"outcome\":\"Moved\"",
                               "\"outcome\":\"Wibbled\""),
                    "outcome-name patch missing");
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(oline), kRootKey);
    REQUIRE_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::WrongValueType);

    // Unknown category name (INTENT line).
    std::string iline = image;
    REQUIRE_MESSAGE(patch_from(iline, intent_line, "\"dest_cat\":\"Documents\"",
                               "\"dest_cat\":\"Finance\""),
                    "category patch missing");
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(iline), kRootKey);
    REQUIRE_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::WrongValueType);
}

TEST_CASE("T-13: run and per-segment continuity are enforced (with a positive control)") {
    const JournalHashResult genesis = journal_genesis_prev(kRootKey, kRootRunSeq, kRun);
    REQUIRE(genesis.ok);
    // Record 0 carries the genesis seed, so the positive control parses.
    const std::string line0 = ser_first(JournalRecord{make_run_start(1, false)});

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
    const std::string seg0 = ser_first(JournalRecord{make_undo_start(1)});
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
    const std::string line0 = ser_first(JournalRecord{make_run_start(1, false)});
    // Second record whose prev does not chain to record 0.
    const JournalSerializeResult bad_line1 =
        serialize_journal_record(JournalRecord{make_intent(1, 0)}, hex64('e'));
    REQUIRE(bad_line1.ok);
    const ParsedJournalFile p1 =
        parse_journal_file(to_bytes(line0 + bad_line1.canonical), kRootKey);
    REQUIRE_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::ChainBreak);

    // Wrong genesis seed: the same journal under a different root_key.
    const std::string image = ser_first(JournalRecord{make_run_start(1, false)});
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
    // either way (cap or hash - both are refusals). The head itself is
    // genesis-valid so the patched count is what the parse meets.
    std::string patched = ser_first(JournalRecord{make_run_start(2, false)});
    REQUIRE(replace_first(patched, "\"planned_move_count\":2", "\"planned_move_count\":1501"));
    CHECK_FALSE(parse_journal_file(to_bytes(patched), kRootKey).valid);
}

TEST_CASE("T-14: oversize input is refused") {
    std::vector<unsigned char> big(journal_max_file_bytes + 1, 'a');
    const ParsedJournalFile p = parse_journal_file(big, kRootKey);
    CHECK_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::OversizeFile);
}

TEST_CASE("T-14: the 8 KiB record cap counts the terminating LF") {
    // The ratified cap covers the stored record including its terminating
    // LF (docs section 5). Pad an otherwise-parseable maximal RUN-START
    // line inside root_path's value to reach the boundary from both sides:
    // at stored size 8,192 the oversized field is what refuses; at 8,193
    // the record cap refuses first.
    JournalRunStart max_start = make_run_start(journal_move_limit, true);
    max_start.run = std::string(journal_run_max_bytes, 'r');
    max_start.root_run_seq = kU64Max;
    max_start.volume = std::string(journal_volume_max_bytes, 'v');
    max_start.root_path = std::string(journal_root_path_max_bytes, 'p');
    max_start.supersedes_root_run_seq = kU64Max;
    const std::string base = ser(JournalRecord{max_start});
    REQUIRE(base.size() == 4831);  // byte pin: maximal RUN-START incl. LF
    const std::size_t pad_at_cap = journal_max_record_bytes - base.size();
    const std::string marker = "\"root_path\":\"";
    const std::size_t found = base.find(marker);
    REQUIRE(found != std::string::npos);
    const std::size_t insert_at = found + marker.size();

    const auto padded = [&](std::size_t pad) {
        std::string line = base;
        line.insert(insert_at, pad, 'p');
        return line;
    };

    // Stored exactly 8,192 (cap inclusive): the record-cap check passes and
    // the oversized root_path field is what refuses.
    const ParsedJournalFile at_cap = parse_journal_file(to_bytes(padded(pad_at_cap)), kRootKey);
    REQUIRE_FALSE(at_cap.valid);
    CHECK(at_cap.failure.kind == ParseFailureKind::CapExceeded);
    CHECK_MESSAGE(at_cap.failure.detail.find("root_path") != std::string::npos,
                  "the field cap is what refuses at the boundary: ", at_cap.failure.detail);

    // Stored 8,193 (one past the cap): the record cap refuses first.
    const ParsedJournalFile over_cap =
        parse_journal_file(to_bytes(padded(pad_at_cap + 1)), kRootKey);
    REQUIRE_FALSE(over_cap.valid);
    CHECK(over_cap.failure.kind == ParseFailureKind::CapExceeded);
    CHECK_MESSAGE(over_cap.failure.detail.find("8 KiB") != std::string::npos,
                  "the record cap is what refuses past the boundary: ",
                  over_cap.failure.detail);
}

TEST_CASE("T-14: an oversize unterminated tail is refused by the record cap") {
    // A ratified writer never emits a record above the cap, so an oversize
    // tail (terminated or not) is corruption: the file is invalid, not
    // merely torn.
    const std::string head = ser_first(JournalRecord{make_run_start(1, false)});
    std::string image = head + std::string(journal_max_record_bytes, 'a');
    const ParsedJournalFile p = parse_journal_file(to_bytes(image), kRootKey);
    CHECK_FALSE(p.valid);
    CHECK(p.failure.kind == ParseFailureKind::CapExceeded);

    // The same ceiling governs anchor records (450 bytes, LF counted).
    const ParsedAnchorFile a1 = parse_anchor_file(to_bytes(std::string(460, 'a')));
    CHECK_FALSE(a1.valid);
    CHECK(a1.failure.kind == ParseFailureKind::CapExceeded);
    const ParsedAnchorFile a2 = parse_anchor_file(to_bytes(std::string(460, 'a') + "\n"));
    CHECK_FALSE(a2.valid);
    CHECK(a2.failure.kind == ParseFailureKind::CapExceeded);

    // The boundary is inclusive: one byte under the cap the tail is merely
    // torn (a garbage tail still fails record validation), never capped.
    std::string under = head + std::string(journal_max_record_bytes - 1, 'a');
    const ParsedJournalFile p_under = parse_journal_file(to_bytes(under), kRootKey);
    REQUIRE(p_under.valid);
    CHECK(p_under.tail == JournalTailKind::Torn);
    const ParsedAnchorFile a0 = parse_anchor_file(to_bytes(std::string(449, 'a')));
    REQUIRE(a0.valid);
    CHECK(a0.tail == JournalTailKind::Torn);
}

// ---------------------------------------------------------------------------
// T-10/T-11: non-canonical encodings and framing.
// ---------------------------------------------------------------------------

TEST_CASE("T-10: non-canonical numbers, escapes and whitespace are rejected") {
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)},
                                           JournalRecord{make_result(2, 0)}});
    const std::size_t result_line = line_offset(image, 2);

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
        std::string patched = image;
        REQUIRE_MESSAGE(patch_from(patched, result_line, bad.from, bad.to),
                        "target missing: ", bad.label);
        const ParsedJournalFile p = parse_journal_file(to_bytes(patched), kRootKey);
        CHECK_MESSAGE(!p.valid, "accepted non-canonical encoding: ", bad.label);
    }

    // Uppercase hex in a hash field (record 0's prev): the hash reader
    // refuses anything but lowercase hex. The first hex digit sits directly
    // after the 8-byte needle; force it into an uppercase (rejected) form
    // whatever digit it is (a-f uppercases, 0-9 becomes 'A').
    std::string upper = image;
    const std::size_t prev_pos = upper.find("\"prev\":\"");
    REQUIRE(prev_pos != std::string::npos);
    const char first = upper[prev_pos + 8];
    upper[prev_pos + 8] =
        (first >= 'a' && first <= 'f') ? static_cast<char>(first - 'a' + 'A') : 'A';
    const ParsedJournalFile p_upper = parse_journal_file(to_bytes(upper), kRootKey);
    CHECK_FALSE(p_upper.valid);
    CHECK(p_upper.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

TEST_CASE("T-11: interior empty lines and CRLF framing are non-canonical") {
    const std::string line = ser_first(JournalRecord{make_run_start(1, false)});
    const ParsedJournalFile empty_line =
        parse_journal_file(to_bytes(line + "\n"), kRootKey);
    CHECK_FALSE(empty_line.valid);
    CHECK(empty_line.failure.kind == ParseFailureKind::NonCanonicalEncoding);
    std::string crlf = line.substr(0, line.size() - 1) + "\r\n";
    const ParsedJournalFile crlf_file =
        parse_journal_file(to_bytes(crlf), kRootKey);
    CHECK_FALSE(crlf_file.valid);
    CHECK(crlf_file.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

// ---------------------------------------------------------------------------
// T-10/T-13: canonical member separators. The parser consumes exactly one
// comma before every member after the first; every deviation is refused.
// ---------------------------------------------------------------------------

TEST_CASE("T-10: missing, double, leading and trailing commas are rejected") {
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)}});
    const std::size_t intent_line = line_offset(image, 1);
    const ParsedJournalFile control = parse_journal_file(to_bytes(image), kRootKey);
    REQUIRE(control.valid);  // positive control

    // Missing comma between two members of the INTENT line.
    std::string missing = image;
    REQUIRE_MESSAGE(patch_from(missing, intent_line,
                               "\"type\":\"INTENT\",\"seq\"", "\"type\":\"INTENT\"\"seq\""),
                    "missing-comma target missing");
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(missing), kRootKey);
    CHECK_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::NonCanonicalEncoding);

    // Double comma.
    std::string double_comma = image;
    REQUIRE_MESSAGE(patch_from(double_comma, intent_line,
                               "\"type\":\"INTENT\",\"seq\"", "\"type\":\"INTENT\",,\"seq\""),
                    "double-comma target missing");
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(double_comma), kRootKey);
    CHECK_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::NonCanonicalEncoding);

    // Leading comma after the opening brace (a comma before the first
    // member is never canonical).
    std::string leading = image;
    REQUIRE(patch_from(leading, 0, "{\"v\":1", "{,\"v\":1"));
    const ParsedJournalFile p3 = parse_journal_file(to_bytes(leading), kRootKey);
    CHECK_FALSE(p3.valid);
    CHECK(p3.failure.kind == ParseFailureKind::NonCanonicalEncoding);

    // Trailing comma before the closing brace of the INTENT record.
    std::string trailing = image;
    REQUIRE(patch_from(trailing, intent_line, "\"}\n", ",\"}\n"));
    const ParsedJournalFile p4 = parse_journal_file(to_bytes(trailing), kRootKey);
    CHECK_FALSE(p4.valid);
    CHECK(p4.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

TEST_CASE("T-13: optional acknowledgement - present, absent, lookahead restored") {
    // Present: the pair serializes and parses.
    const ParsedJournalFile with_ack =
        parse_ok(ser_first(JournalRecord{make_run_start(2, true)}));
    REQUIRE(with_ack.records.size() == 1);
    CHECK(std::get<JournalRunStart>(with_ack.records[0]).has_ack);

    // Absent: the failed lookahead for the optional member must leave the
    // parser fully restored (offset, error, failure kind, member count), so
    // the following prev/integrity members still parse.
    const ParsedJournalFile without_ack =
        parse_ok(ser_first(JournalRecord{make_run_start(2, false)}));
    REQUIRE(without_ack.records.size() == 1);
    CHECK_FALSE(std::get<JournalRunStart>(without_ack.records[0]).has_ack);
}

TEST_CASE("T-10: commas inside string values do not disturb member framing") {
    JournalResult commas = make_result(2, 0, JournalOutcome::Moved, "a,b, c ,, \"d,e\"");
    const ParsedJournalFile p =
        parse_ok(chain_image({JournalRecord{make_run_start(1, false)},
                              JournalRecord{make_intent(1, 0)},
                              JournalRecord{commas}}));
    REQUIRE(p.records.size() == 3);
    CHECK(p.records[2] == JournalRecord{commas});
}

// ---------------------------------------------------------------------------
// T-15: prev/integrity wire order, genesis and chain seeds, byte accounting.
// ---------------------------------------------------------------------------

TEST_CASE("T-15: prev precedes integrity; genesis and chain seeds are exact") {
    // Record 0: prev is the genesis seed of its own run/root_run_seq.
    const std::string line0 = ser_first(JournalRecord{make_run_start(1, false)});
    const JournalHashResult genesis = journal_genesis_prev(kRootKey, kRootRunSeq, kRun);
    REQUIRE(genesis.ok);
    const std::size_t prev_field = line0.find("\"prev\":\"");
    REQUIRE(prev_field != std::string::npos);
    CHECK(line0.compare(prev_field + 8, 64, genesis.hex) == 0);

    // Wire order: "prev" is immediately followed by its 64-hex value, the
    // closing quote and the "integrity" member; the line ends `"}\n`.
    const std::string tail = line0.substr(prev_field);
    const std::string expected_prefix =
        "\"prev\":\"" + genesis.hex + "\",\"integrity\":\"";
    REQUIRE(tail.size() >= expected_prefix.size() + 64 + 3);
    CHECK(tail.compare(0, expected_prefix.size(), expected_prefix) == 0);
    CHECK(tail.compare(tail.size() - 3, 3, "\"}\n") == 0);

    // Record 1: prev is SHA-256 over record 0's full canonical bytes
    // (integrity included, LF excluded).
    const std::string line1 =
        serialize_journal_record(JournalRecord{make_intent(1, 0)}, prev_of(line0))
            .canonical;
    const std::size_t prev1 = line1.find("\"prev\":\"");
    REQUIRE(prev1 != std::string::npos);
    CHECK(line1.compare(prev1 + 8, 64, prev_of(line0)) == 0);
    CHECK(parse_ok(line0 + line1).records.size() == 2);
}

TEST_CASE("T-15: integrity covers prev - tampering with prev is detected") {
    const std::string image = chain_image({JournalRecord{make_run_start(1, false)},
                                           JournalRecord{make_intent(1, 0)}});
    const ParsedJournalFile good = parse_journal_file(to_bytes(image), kRootKey);
    REQUIRE(good.valid);  // positive control

    // Replace record 1's prev value with different (still canonical) hex:
    // record 1's integrity hash was computed over a body that includes
    // prev, so the tampering must surface as a chain break.
    std::string tampered = image;
    const std::size_t first_lf = tampered.find('\n');
    REQUIRE(first_lf != std::string::npos);
    const std::size_t prev2 = tampered.find("\"prev\":\"", first_lf);
    REQUIRE(prev2 != std::string::npos);
    tampered.replace(prev2 + 8, 64, hex64('a'));
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(tampered), kRootKey);
    CHECK_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::ChainBreak);
}

TEST_CASE("T-15: a record without prev, or with prev out of order, is rejected") {
    const std::string line0 = ser_first(JournalRecord{make_run_start(1, false)});

    // Missing prev: erase the member (9 key/quote bytes + 64 hex + 1 quote)
    // from an otherwise valid record 0.
    std::string missing = line0;
    const std::size_t prev_field = missing.find(",\"prev\":\"");
    REQUIRE(prev_field != std::string::npos);
    REQUIRE(missing.compare(prev_field + 9 + 64, 1, "\"") == 0);
    missing.erase(prev_field, 74);
    const ParsedJournalFile p1 = parse_journal_file(to_bytes(missing), kRootKey);
    CHECK_FALSE(p1.valid);
    CHECK(p1.failure.kind == ParseFailureKind::NonCanonicalEncoding);

    // Reordered prev/integrity: `prev` must be the second-to-last member.
    std::string swapped = line0;
    const std::size_t pf = swapped.find(",\"prev\":\"");
    REQUIRE(pf != std::string::npos);
    const std::size_t inf = swapped.find(",\"integrity\":\"", pf);
    REQUIRE(inf != std::string::npos);
    const std::string prev_member = swapped.substr(pf, 74);
    const std::string integrity_member = swapped.substr(inf, 79);
    swapped.replace(pf, (inf - pf) + 79, integrity_member + prev_member);
    const ParsedJournalFile p2 = parse_journal_file(to_bytes(swapped), kRootKey);
    CHECK_FALSE(p2.valid);
    CHECK(p2.failure.kind == ParseFailureKind::NonCanonicalEncoding);
}

TEST_CASE("T-15: the prev member is exactly 74 bytes, before integrity") {
    // Byte-level pin: `,"prev":"` (9) + 64 hex + `"` (1) = 74 bytes, inserted
    // immediately before `,"integrity":"`. Anchor records carry no prev.
    const std::string line = ser_first(JournalRecord{make_run_start(2, false)});
    const std::size_t prev_at = line.find(",\"prev\":\"");
    const std::size_t integrity_at = line.find(",\"integrity\":\"");
    REQUIRE(prev_at != std::string::npos);
    REQUIRE(integrity_at != std::string::npos);
    CHECK(integrity_at - prev_at == 74);

    // Full-line byte pin derived field-by-field from the canonical schema
    // for make_run_start(2, false); volume/root_path JSON escaping counted
    // (4 and 3 backslashes respectively, each serialized as `\\`).
    const std::string run = kRun;
    const std::string volume = kVolume;       // 49 content bytes
    const std::string root_path = kRootPath;  // 21 content bytes
    const std::size_t expected =
        6                                    // {"v":1
        + 8 + run.size() + 1                 // ,"run":"<run>"
        + 16                                 // ,"dir":"forward"
        + 19                                 // ,"type":"RUN-START"
        + 7 + 1                              // ,"seq":0
        + 16 + 1                             // ,"root_run_seq":7
        + 22 + 1                             // ,"planned_move_count":2
        + 12 + 73 + 1                        // ,"lock_id":"<73>"
        + 12 + 73 + 1                        // ,"root_id":"<73>"
        + 14 + 73 + 1                        // ,"anchor_id":"<73>"
        + 11 + volume.size() + 4 + 1         // ,"volume":"<volume + 4 escapes>"
        + 14 + root_path.size() + 3 + 1      // ,"root_path":"<path + 3 escapes>"
        + 9 + 64 + 1                         // ,"prev":"<64>"
        + 14 + 64 + 1                        // ,"integrity":"<64>"
        + 1 + 1;                             // '}' and the terminating LF
    CHECK(line.size() == expected);
    CHECK(line.back() == '\n');
}

// ---------------------------------------------------------------------------
// T-15: canonical stability, hash-chain wiring, universal bit-flip detection.
// ---------------------------------------------------------------------------

TEST_CASE("T-15: canonical serialization is stable and the hash chain is wired") {
    // A valid two-move run: planned_move_count matches the two INTENT/RESULT
    // pairs and every record's seq equals its position (the pre-remediation
    // fixture put run_end(5, 2) at index 3, which no ratified writer emits).
    const std::vector<JournalRecord> records = {
        JournalRecord{make_run_start(2, true)},
        JournalRecord{make_intent(1, 0)},
        JournalRecord{make_result(2, 0)},
        JournalRecord{make_intent(3, 1)},
        JournalRecord{make_result(4, 1)},
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

    // The document's UNDO-INTENT/UNDO-RESULT/UNDO-END (and, at the 20-digit
    // root_run_seq ceiling, RUN-END) per-record estimates are not satisfied
    // by the corrected canonical encoding: true schema maxima are 2,326 /
    // 2,881 / 537 / 411 bytes incl. LF. Hosted run 7 measured the fixtures
    // at 2,250 / 2,805 / 402 / 295 before this remediation; the corrected
    // encoding drops the spurious move_id member from undo records (-19 at
    // the 8-digit maximum) and adds the 74-byte prev member, plus maximal
    // run/root_run_seq width deltas. The 8 KiB record cap above is the
    // binding runtime constraint; the docs section 12 table is corrected in
    // the documentation-only pass (no runtime restriction is derived from
    // the estimates).
    // Byte pin: hosted run #7 measured this UNDO-END fixture at 402 bytes
    // before `prev` was serialized; the fix adds exactly 74.
    CHECK(undo_end == 476);
    CHECK(ser(JournalRecord{make_undo_end(journal_seq_max, journal_seq_max)}).back() == '\n');

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

    // Exact-byte pin for a one-candidate attempt (regression pin for the
    // moved-from reserve fix, which previously read moved-from strings):
    // UNDO-START 486 + maximal UNDO-INTENT 2,265 + maximal UNDO-RESULT 2,820
    // + UNDO-END 455; every record includes its 74-byte prev member and the
    // terminating LF, with maximal identities/names/timestamps/detail. Undo
    // records carry no move_id member (docs section 5).
    const CapacityEstimate small = segment_reserve_bytes(make_undo_start(1), 1);
    REQUIRE_MESSAGE(small.ok, "small segment reserve failed: ", small.error);
    CHECK(small.bytes == 6026);
    const CapacityEstimate anc = anchor_reserve_bytes(journal_move_limit);
    REQUIRE_MESSAGE(anc.ok, "anchor reserve failed: ", anc.error);

    MESSAGE("exact reserves at N=1,500: forward=", fwd.bytes, " segment=", seg.bytes,
            " anchor=", anc.bytes, " bytes");
    CHECK(within_per_file_limit(fwd.bytes));
    CHECK(within_per_file_limit(seg.bytes));
    CHECK(within_per_file_limit(anc.bytes));

    // The ratified 128 MiB aggregate admits 8 complete cycles (docs section
    // 12); a 9th cycle exceeds it, and admission is exact to the byte.
    const std::size_t cycle = fwd.bytes + anc.bytes + seg.bytes;
    MESSAGE("one worst-case cycle = ", cycle, " bytes; 8 cycles = ", 8 * cycle);
    CHECK(8 * cycle <= journal_max_aggregate_bytes);
    CHECK(within_aggregate_limit(7 * cycle, cycle));
    CHECK_FALSE(within_aggregate_limit(8 * cycle, cycle));
    CHECK_FALSE(within_aggregate_limit(journal_max_aggregate_bytes, 1));
}
