// FolderPilot - Phase 7 move journal: canonical codec and strict parser.
//
// Canonical form (pinned; see journal.hpp for the convention list):
//   * JSONL - one record per line, LF-terminated, no whitespace anywhere
//     outside strings, no CRLF,
//   * fixed key order per record type; `integrity` is always the last
//     member, `prev` the second-to-last,
//   * strings: minimal escaping (`\"`, `\\`, `\b`, `\f`, `\n`, `\r`, `\t`,
//     and `\u00xx` with lowercase hex for the remaining C0 controls),
//   * numbers: non-negative integers without leading zeros, sign, fraction
//     or exponent,
//   * hashes: exactly 64 lowercase hex characters, no prefix,
//   * integrity = SHA-256 over the record's canonical bytes with the
//     `integrity` member omitted (the closing brace re-appended); the
//     terminating LF is not hashed,
//   * prev = SHA-256 over the previous record's full canonical bytes
//     including its `integrity` member (genesis: root-key || root_run_seq
//     || run).
//
// The parser validates the complete record stream of a file: per-record
// schema and caps, per-record integrity, the prev chain, the expected-record
// state machine (docs section 6), continuity of run/root_run_seq/dir/
// fwd_journal_digest, and the three-state tail (terminated record /
// unterminated-but-complete record / torn bytes with a best-effort type
// hint). It never touches the filesystem.

#include "folderpilot/journal.hpp"

#include "sha256.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace folderpilot {

namespace {

constexpr std::size_t kDirNameMax = 9;
constexpr std::size_t kTypeNameMax = 13;
constexpr std::size_t kOutcomeNameMax = 20;
constexpr std::size_t kCategoryNameMax = 11;
constexpr unsigned long long kU64Max = 18446744073709551615ULL;

constexpr char kHexDigits[] = "0123456789abcdef";

bool is_hex_lower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

std::string hex_of(std::span<const unsigned char> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

// Full UTF-8 validation: rejects overlong encodings, UTF-8-encoded
// surrogates, code points above U+10FFFF, and truncated sequences.
bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned long long cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1FU;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0FU;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07U;
        } else {
            return false;  // stray continuation byte or invalid lead
        }
        if (i + extra >= s.size()) {
            return false;  // truncated sequence
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3FU);
        }
        if (extra == 1 && cp < 0x80) return false;     // overlong
        if (extra == 2 && cp < 0x800) return false;    // overlong
        if (extra == 3 && cp < 0x10000) return false;  // overlong
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

void append_u64(std::string& out, unsigned long long v) {
    char buf[20];
    std::size_t n = 0;
    do {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        out.push_back(buf[--n]);
    }
}

// Minimal canonical JSON string escaping. Input must already be valid UTF-8.
void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case 0x08: out += "\\b"; break;
            case 0x0C: out += "\\f"; break;
            case 0x0A: out += "\\n"; break;
            case 0x0D: out += "\\r"; break;
            case 0x09: out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHexDigits[c >> 4]);
                    out.push_back(kHexDigits[c & 0x0F]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

// ---------------------------------------------------------------------------
// Field validators (serialization side).
// ---------------------------------------------------------------------------

bool check_nonempty_utf8(std::string& error, std::string_view v, std::size_t cap,
                         std::string_view what) {
    if (v.empty()) {
        error = std::string(what) + " must not be empty";
        return false;
    }
    if (v.size() > cap) {
        error = std::string(what) + " exceeds its " + std::to_string(cap) + "-byte cap";
        return false;
    }
    if (!valid_utf8(v)) {
        error = std::string(what) + " is not valid UTF-8";
        return false;
    }
    return true;
}

bool check_hex64(std::string& error, std::string_view v, std::string_view what) {
    if (v.size() != journal_hex_chars) {
        error = std::string(what) + " must be exactly 64 hex characters";
        return false;
    }
    for (const char c : v) {
        if (!is_hex_lower(c)) {
            error = std::string(what) + " must be lowercase hex";
            return false;
        }
    }
    return true;
}

bool check_identity(std::string& error, std::string_view v, std::string_view what) {
    return check_nonempty_utf8(error, v, journal_identity_max_bytes, what);
}

bool check_name(std::string& error, std::string_view v, std::string_view what) {
    if (!check_nonempty_utf8(error, v, journal_name_max_bytes, what)) {
        return false;
    }
    // Filenames are single components: '"' and '\' cannot occur and control
    // characters would need escaping, which the canonical form never
    // produces for names [W32-DOC].
    for (const char ch : v) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == '"' || c == '\\') {
            error = std::string(what) +
                    " must be a single component without quotes, backslashes "
                    "or control characters";
            return false;
        }
    }
    return true;
}

bool valid_category(Category c) {
    for (const Category known : all_categories) {
        if (known == c) {
            return true;
        }
    }
    return false;
}

std::optional<Category> category_from_name(std::string_view name) {
    for (const Category known : all_categories) {
        if (category_name(known) == name) {
            return known;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Record body builders. Each produces the canonical bytes WITHOUT the
// `integrity` member but WITH the closing brace.
// ---------------------------------------------------------------------------

struct Body {
    bool ok = false;
    std::string bytes;
    std::string error;
};

bool append_open(std::string& out, std::string& error, std::string_view run, JournalDir dir,
                 std::string_view type_name, unsigned long long seq,
                 unsigned long long root_run_seq) {
    if (!check_nonempty_utf8(error, run, journal_run_max_bytes, "run")) {
        return false;
    }
    if (seq > journal_seq_max) {
        error = "seq exceeds the 8-digit ceiling";
        return false;
    }
    out += "{\"v\":1,\"run\":";
    append_json_string(out, run);
    out += ",\"dir\":";
    append_json_string(out, dir == JournalDir::Forward ? "forward" : "undo");
    out += ",\"type\":";
    append_json_string(out, type_name);
    out += ",\"seq\":";
    append_u64(out, seq);
    out += ",\"root_run_seq\":";
    append_u64(out, root_run_seq);
    return true;
}

bool append_move_evidence(std::string& out, std::string& error, bool with_move_id,
                          unsigned long long move_id,
                          const std::string& cat_id, const std::string& file_id,
                          unsigned long long file_ctime, unsigned long long file_size,
                          unsigned long long file_mtime, const std::string& source_name,
                          const std::string& dest_name, Category dest_cat) {
    // Undo records identify the reversed move with undo_of_move alone; the
    // move_id member exists on forward INTENT/RESULT only (docs section 5,
    // undo-attempt schema).
    if (with_move_id) {
        if (move_id > journal_seq_max) {
            error = "move id exceeds the 8-digit ceiling";
            return false;
        }
        out += ",\"move_id\":";
        append_u64(out, move_id);
    }
    if (!check_identity(error, cat_id, "cat_id")) {
        return false;
    }
    if (!check_identity(error, file_id, "file_id")) {
        return false;
    }
    if (!check_name(error, source_name, "source_name")) {
        return false;
    }
    if (!check_name(error, dest_name, "dest_name")) {
        return false;
    }
    if (!valid_category(dest_cat)) {
        error = "dest_cat is not a valid category";
        return false;
    }
    out += ",\"cat_id\":";
    append_json_string(out, cat_id);
    out += ",\"file_id\":";
    append_json_string(out, file_id);
    out += ",\"file_ctime\":";
    append_u64(out, file_ctime);
    out += ",\"file_size\":";
    append_u64(out, file_size);
    out += ",\"file_mtime\":";
    append_u64(out, file_mtime);
    out += ",\"source_name\":";
    append_json_string(out, source_name);
    out += ",\"dest_name\":";
    append_json_string(out, dest_name);
    out += ",\"dest_cat\":";
    append_json_string(out, category_name(dest_cat));
    return true;
}

bool check_summary_counts(std::string& error, unsigned long long planned,
                          unsigned long long moved, unsigned long long conflict,
                          unsigned long long skipped, unsigned long long failed) {
    const unsigned long long counts[] = {planned, moved, conflict, skipped, failed};
    for (const unsigned long long c : counts) {
        if (c > journal_seq_max) {
            error = "summary count exceeds the 8-digit ceiling";
            return false;
        }
    }
    return true;
}

void append_summary_counts(std::string& out, unsigned long long planned,
                           unsigned long long moved, unsigned long long conflict,
                           unsigned long long skipped, unsigned long long failed) {
    out += ",\"planned_count\":";
    append_u64(out, planned);
    out += ",\"moved_count\":";
    append_u64(out, moved);
    out += ",\"conflict_count\":";
    append_u64(out, conflict);
    out += ",\"skipped_count\":";
    append_u64(out, skipped);
    out += ",\"failed_count\":";
    append_u64(out, failed);
}

bool append_outcome_detail(std::string& out, std::string& error, JournalOutcome outcome,
                           bool allow_restored, const std::string& detail) {
    const std::string_view name = journal_outcome_name(outcome);
    if (name.empty()) {
        error = "outcome is outside the ratified vocabulary";
        return false;
    }
    if (outcome == JournalOutcome::Restored && !allow_restored) {
        error = "RESULT cannot record the undo outcome 'restored'";
        return false;
    }
    if (detail.size() > journal_detail_max_bytes) {
        error = "detail exceeds its 512-byte cap";
        return false;
    }
    if (!valid_utf8(detail)) {
        error = "detail is not valid UTF-8";
        return false;
    }
    out += ",\"outcome\":";
    append_json_string(out, name);
    out += ",\"detail\":";
    append_json_string(out, detail);
    return true;
}

Body build_body(const JournalRunStart& r) {
    Body b;
    std::string& out = b.bytes;
    if (r.seq != 0) {
        b.error = "RUN-START seq must be 0";
        return b;
    }
    if (r.planned_move_count > journal_move_limit) {
        b.error = "planned_move_count exceeds the 1,500-move operational limit";
        return b;
    }
    if (r.has_ack && !r.undo_window_acknowledged) {
        b.error = "the acknowledgement pair serializes only as true (docs section 8)";
        return b;
    }
    if (!append_open(out, b.error, r.run, JournalDir::Forward, "RUN-START", r.seq,
                     r.root_run_seq)) {
        return b;
    }
    if (!check_nonempty_utf8(b.error, r.lock_id, journal_identity_max_bytes, "lock_id") ||
        !check_nonempty_utf8(b.error, r.root_id, journal_identity_max_bytes, "root_id") ||
        !check_nonempty_utf8(b.error, r.anchor_id, journal_identity_max_bytes, "anchor_id") ||
        !check_nonempty_utf8(b.error, r.volume, journal_volume_max_bytes, "volume") ||
        !check_nonempty_utf8(b.error, r.root_path, journal_root_path_max_bytes, "root_path")) {
        return b;
    }
    out += ",\"planned_move_count\":";
    append_u64(out, r.planned_move_count);
    out += ",\"lock_id\":";
    append_json_string(out, r.lock_id);
    out += ",\"root_id\":";
    append_json_string(out, r.root_id);
    out += ",\"anchor_id\":";
    append_json_string(out, r.anchor_id);
    out += ",\"volume\":";
    append_json_string(out, r.volume);
    out += ",\"root_path\":";
    append_json_string(out, r.root_path);
    if (r.has_ack) {
        out += ",\"undo_window_acknowledged\":true";
        out += ",\"supersedes_root_run_seq\":";
        append_u64(out, r.supersedes_root_run_seq);
    }
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const JournalIntent& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Forward, "INTENT", r.seq, r.root_run_seq)) {
        return b;
    }
    if (!append_move_evidence(out, b.error, true, r.move_id, r.cat_id, r.file_id, r.file_ctime,
                              r.file_size, r.file_mtime, r.source_name, r.dest_name,
                              r.dest_cat)) {
        return b;
    }
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const JournalResult& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Forward, "RESULT", r.seq, r.root_run_seq)) {
        return b;
    }
    if (!append_move_evidence(out, b.error, true, r.move_id, r.cat_id, r.file_id, r.file_ctime,
                              r.file_size, r.file_mtime, r.source_name, r.dest_name,
                              r.dest_cat)) {
        return b;
    }
    if (!append_outcome_detail(out, b.error, r.outcome, false, r.detail)) {
        return b;
    }
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const JournalRunEnd& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Forward, "RUN-END", r.seq, r.root_run_seq)) {
        return b;
    }
    if (!check_summary_counts(b.error, r.planned_count, r.moved_count, r.conflict_count,
                              r.skipped_count, r.failed_count)) {
        return b;
    }
    append_summary_counts(out, r.planned_count, r.moved_count, r.conflict_count,
                          r.skipped_count, r.failed_count);
    out += '}';
    b.ok = true;
    return b;
}

bool append_undo_mid(std::string& out, std::string& error,
                     const std::string& fwd_journal_digest, unsigned long long undo_attempt_seq) {
    if (!check_hex64(error, fwd_journal_digest, "fwd_journal_digest")) {
        return false;
    }
    out += ",\"fwd_journal_digest\":";
    append_json_string(out, fwd_journal_digest);
    out += ",\"undo_attempt_seq\":";
    append_u64(out, undo_attempt_seq);
    return true;
}

Body build_body(const JournalUndoStart& r) {
    Body b;
    std::string& out = b.bytes;
    if (r.seq != 0) {
        b.error = "UNDO-START seq must be 0";
        return b;
    }
    if (r.attempt_candidate_count == 0 || r.attempt_candidate_count > journal_move_limit) {
        b.error = "attempt_candidate_count must be within 1..1500";
        return b;
    }
    if (!append_open(out, b.error, r.run, JournalDir::Undo, "UNDO-START", r.seq,
                     r.root_run_seq)) {
        return b;
    }
    if (!append_undo_mid(out, b.error, r.fwd_journal_digest, r.undo_attempt_seq)) {
        return b;
    }
    if (!check_identity(b.error, r.journal_id, "journal_id")) {
        return b;
    }
    out += ",\"journal_id\":";
    append_json_string(out, r.journal_id);
    out += ",\"attempt_candidate_count\":";
    append_u64(out, r.attempt_candidate_count);
    out += '}';
    b.ok = true;
    return b;
}

bool append_undo_move_ids(std::string& out, std::string& error,
                          unsigned long long attempt_move_seq, unsigned long long undo_of_move) {
    if (attempt_move_seq > journal_seq_max || undo_of_move > journal_seq_max) {
        error = "attempt move id exceeds the 8-digit ceiling";
        return false;
    }
    out += ",\"attempt_move_seq\":";
    append_u64(out, attempt_move_seq);
    out += ",\"undo_of_move\":";
    append_u64(out, undo_of_move);
    return true;
}

Body build_body(const JournalUndoIntent& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Undo, "UNDO-INTENT", r.seq,
                     r.root_run_seq)) {
        return b;
    }
    if (!append_undo_mid(out, b.error, r.fwd_journal_digest, r.undo_attempt_seq)) {
        return b;
    }
    if (!append_undo_move_ids(out, b.error, r.attempt_move_seq, r.undo_of_move)) {
        return b;
    }
    if (!append_move_evidence(out, b.error, false, r.undo_of_move, r.cat_id, r.file_id, r.file_ctime,
                              r.file_size, r.file_mtime, r.source_name, r.dest_name,
                              r.dest_cat)) {
        return b;
    }
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const JournalUndoResult& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Undo, "UNDO-RESULT", r.seq,
                     r.root_run_seq)) {
        return b;
    }
    if (!append_undo_mid(out, b.error, r.fwd_journal_digest, r.undo_attempt_seq)) {
        return b;
    }
    if (!append_undo_move_ids(out, b.error, r.attempt_move_seq, r.undo_of_move)) {
        return b;
    }
    if (!append_move_evidence(out, b.error, false, r.undo_of_move, r.cat_id, r.file_id, r.file_ctime,
                              r.file_size, r.file_mtime, r.source_name, r.dest_name,
                              r.dest_cat)) {
        return b;
    }
    if (!append_outcome_detail(out, b.error, r.outcome, true, r.detail)) {
        return b;
    }
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const JournalUndoEnd& r) {
    Body b;
    std::string& out = b.bytes;
    if (!append_open(out, b.error, r.run, JournalDir::Undo, "UNDO-END", r.seq, r.root_run_seq)) {
        return b;
    }
    if (!append_undo_mid(out, b.error, r.fwd_journal_digest, r.undo_attempt_seq)) {
        return b;
    }
    if (!check_summary_counts(b.error, r.planned_count, r.moved_count, r.conflict_count,
                              r.skipped_count, r.failed_count)) {
        return b;
    }
    append_summary_counts(out, r.planned_count, r.moved_count, r.conflict_count,
                          r.skipped_count, r.failed_count);
    out += '}';
    b.ok = true;
    return b;
}

Body build_body(const AnchorRecord& r) {
    Body b;
    std::string& out = b.bytes;
    if (!check_identity(b.error, r.journal_id, "journal_id")) {
        return b;
    }
    if (!check_hex64(b.error, r.last_record_hash, "last_record_hash")) {
        return b;
    }
    if (r.anchor_seq > journal_seq_max || r.covered_seq > journal_seq_max) {
        b.error = "anchor_seq/covered_seq exceed the 8-digit ceiling";
        return b;
    }
    out += "{\"v\":1,\"kind\":\"anchor\",\"root_run_seq\":";
    append_u64(out, r.root_run_seq);
    out += ",\"file_kind\":\"journal\",\"journal_id\":";
    append_json_string(out, r.journal_id);
    out += ",\"anchor_seq\":";
    append_u64(out, r.anchor_seq);
    out += ",\"covered_seq\":";
    append_u64(out, r.covered_seq);
    out += ",\"byte_len\":";
    append_u64(out, r.byte_len);
    out += ",\"last_record_hash\":";
    append_json_string(out, r.last_record_hash);
    out += '}';
    b.ok = true;
    return b;
}

// ---------------------------------------------------------------------------
// Strict line parser.
// ---------------------------------------------------------------------------

struct StringRules {
    std::size_t max_bytes = 0;
    bool allow_empty = false;
    bool forbid_escapes = false;   // names/type/dir: canonical form never escapes them
    bool forbid_controls = false;  // names/type/dir: raw control bytes rejected
};

StringRules identity_rules() {
    StringRules r;
    r.max_bytes = journal_identity_max_bytes;
    return r;
}

StringRules name_rules() {
    StringRules r;
    r.max_bytes = journal_name_max_bytes;
    r.forbid_escapes = true;
    r.forbid_controls = true;
    return r;
}

struct LineParser {
    std::string_view line;
    std::size_t line_start = 0;   // offset of the line within the file image
    std::string error;
    ParseFailureKind kind = ParseFailureKind::NonCanonicalEncoding;
    std::size_t offset = 0;       // within the line
    std::vector<std::string_view> seen_keys;
    std::size_t members_seen = 0; // members accepted so far in this record
    const std::vector<std::string_view>* schema_keys = nullptr;

    bool fail(ParseFailureKind k, std::string message, std::size_t line_pos) {
        if (error.empty()) {
            kind = k;
            error = std::move(message);
            offset = line_pos;
        }
        return false;
    }

    ParseFailure failure_at_current() const {
        ParseFailure f;
        f.kind = kind;
        f.offset = line_start + offset;
        f.detail = error;
        return f;
    }

    bool at_end() const { return offset >= line.size(); }

    bool consume(char c) {
        if (offset < line.size() && line[offset] == c) {
            ++offset;
            return true;
        }
        return false;
    }

    bool expect(char c, const char* what = "character") {
        if (consume(c)) {
            return true;
        }
        std::string msg = "expected ";
        msg += what;
        msg += " at this position";
        return fail(ParseFailureKind::NonCanonicalEncoding, std::move(msg), offset);
    }

    // Reads `","` before every member after the first, then `"<name>"` and
    // `':'`. Canonical members are exactly comma-separated: missing, double,
    // leading and trailing commas all fail here. On mismatch the failure is
    // classified: duplicate, misordered, or unknown key.
    bool expect_key(std::string_view name) {
        const std::size_t key_start = offset;
        if (members_seen != 0 && !expect(',', "',' between members")) {
            return false;
        }
        if (!consume('"')) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "expected a key string",
                        key_start);
        }
        const std::size_t name_start = offset;
        while (offset < line.size() && line[offset] != '"') {
            if (line[offset] == '\\') {
                return fail(ParseFailureKind::NonCanonicalEncoding,
                            "escape sequence in a key name", offset);
            }
            ++offset;
        }
        if (offset >= line.size()) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "unterminated key", name_start);
        }
        const std::string_view observed = line.substr(name_start, offset - name_start);
        ++offset;  // closing quote
        if (observed != name) {
            for (const std::string_view k : seen_keys) {
                if (k == observed) {
                    std::string msg = "duplicate key \"";
                    msg += observed;
                    msg += "\"";
                    return fail(ParseFailureKind::DuplicateKey, std::move(msg), key_start);
                }
            }
            if (schema_keys != nullptr) {
                for (const std::string_view k : *schema_keys) {
                    if (k == observed) {
                        std::string msg = "misordered key \"";
                        msg += observed;
                        msg += "\" (expected \"";
                        msg += name;
                        msg += "\")";
                        return fail(ParseFailureKind::NonCanonicalEncoding, std::move(msg),
                                    key_start);
                    }
                }
            }
            std::string msg = "unknown key \"";
            msg += observed;
            msg += "\"";
            return fail(ParseFailureKind::UnknownField, std::move(msg), key_start);
        }
        if (!consume(':')) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "expected ':' after key",
                        offset);
        }
        seen_keys.push_back(observed);
        ++members_seen;
        return true;
    }

    // Peeks for a key without consuming on mismatch. A failed lookahead
    // restores every piece of parser state: offset, error, failure kind and
    // the member count (`seen_keys` only ever grows on success, so it needs
    // no restore).
    bool try_expect_key(std::string_view name) {
        const std::size_t saved_offset = offset;
        const std::size_t saved_members = members_seen;
        const std::string saved_error = error;
        const ParseFailureKind saved_kind = kind;
        if (expect_key(name)) {
            return true;
        }
        offset = saved_offset;
        members_seen = saved_members;
        error = saved_error;
        kind = saved_kind;
        return false;
    }

    bool read_u64_value(unsigned long long& v, unsigned long long max_value) {
        const std::size_t start = offset;
        unsigned long long value = 0;
        std::size_t digits = 0;
        while (offset < line.size() && line[offset] >= '0' && line[offset] <= '9') {
            const unsigned d = static_cast<unsigned>(line[offset] - '0');
            if (value > (kU64Max - d) / 10) {
                return fail(ParseFailureKind::CapExceeded, "number exceeds 2^64-1", start);
            }
            value = value * 10 + d;
            ++digits;
            ++offset;
        }
        if (digits == 0) {
            return fail(ParseFailureKind::WrongValueType,
                        "expected a non-negative integer", start);
        }
        if (digits > 1 && line[start] == '0') {
            return fail(ParseFailureKind::NonCanonicalEncoding, "leading zero in number", start);
        }
        if (value > max_value) {
            return fail(ParseFailureKind::CapExceeded, "number exceeds its ratified ceiling",
                        start);
        }
        v = value;
        return true;
    }

    // Reads a quoted 64-lowercase-hex value (no escapes, no controls).
    bool read_hex64_value(std::string& out) {
        const std::size_t start = offset;
        if (!consume('"')) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "expected hash string", start);
        }
        std::size_t end = start + 1;
        while (end < line.size() && line[end] != '"') {
            const unsigned char c = static_cast<unsigned char>(line[end]);
            if (line[end] == '\\' || c < 0x20) {
                return fail(ParseFailureKind::NonCanonicalEncoding,
                            "hash fields contain raw lowercase hex only", end);
            }
            ++end;
        }
        if (end >= line.size()) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "unterminated hash string",
                        start);
        }
        const std::string_view value = line.substr(start + 1, end - start - 1);
        if (value.size() != journal_hex_chars) {
            return fail(ParseFailureKind::CapExceeded,
                        "hash field must be exactly 64 hex characters", start);
        }
        for (const char c : value) {
            if (!is_hex_lower(c)) {
                return fail(ParseFailureKind::NonCanonicalEncoding,
                            "hash field must be lowercase hex", start);
            }
        }
        out.assign(value);
        offset = end + 1;
        return true;
    }

    bool read_string_value(std::string& out, const StringRules& rules, const char* what) {
        const std::size_t start = offset;
        if (!consume('"')) {
            return fail(ParseFailureKind::NonCanonicalEncoding, "expected string", start);
        }
        std::string value;
        while (offset < line.size()) {
            const char ch = line[offset];
            if (ch == '"') {
                ++offset;
                if (!rules.allow_empty && value.empty()) {
                    std::string msg = what;
                    msg += " must not be empty";
                    return fail(ParseFailureKind::CapExceeded, std::move(msg), start);
                }
                if (value.size() > rules.max_bytes) {
                    std::string msg = what;
                    msg += " exceeds its byte cap";
                    return fail(ParseFailureKind::CapExceeded, std::move(msg), start);
                }
                if (!valid_utf8(value)) {
                    std::string msg = what;
                    msg += " is not valid UTF-8";
                    return fail(ParseFailureKind::InvalidUtf8, std::move(msg), start);
                }
                out = std::move(value);
                return true;
            }
            if (ch == '\\') {
                if (rules.forbid_escapes) {
                    std::string msg = what;
                    msg += " canonical form never contains escapes";
                    return fail(ParseFailureKind::NonCanonicalEncoding, std::move(msg), offset);
                }
                ++offset;
                if (offset >= line.size()) {
                    return fail(ParseFailureKind::NonCanonicalEncoding, "truncated escape",
                                offset);
                }
                const char esc = line[offset];
                if (esc == 'u') {
                    if (offset + 4 >= line.size() || line[offset + 1] != '0' ||
                        line[offset + 2] != '0' || !is_hex_lower(line[offset + 3]) ||
                        !is_hex_lower(line[offset + 4])) {
                        return fail(ParseFailureKind::NonCanonicalEncoding,
                                    "only \\u00xx (lowercase hex) escapes are canonical",
                                    offset);
                    }
                    const unsigned hi = static_cast<unsigned>(
                        line[offset + 3] <= '9' ? line[offset + 3] - '0'
                                                : line[offset + 3] - 'a' + 10);
                    const unsigned lo = static_cast<unsigned>(
                        line[offset + 4] <= '9' ? line[offset + 4] - '0'
                                                : line[offset + 4] - 'a' + 10);
                    const unsigned cp = (hi << 4) | lo;
                    if (cp >= 0x20 || cp == 0x08 || cp == 0x09 || cp == 0x0A || cp == 0x0C ||
                        cp == 0x0D) {
                        return fail(ParseFailureKind::NonCanonicalEncoding,
                                    "\\u00xx escapes are canonical only for C0 controls "
                                    "without short forms",
                                    offset);
                    }
                    value.push_back(static_cast<char>(cp));
                    offset += 5;
                    continue;
                }
                switch (esc) {
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default:
                        return fail(ParseFailureKind::NonCanonicalEncoding,
                                    "unknown escape sequence", offset);
                }
                ++offset;
                continue;
            }
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20) {
                if (rules.forbid_controls) {
                    std::string msg = what;
                    msg += " canonical form never contains control characters";
                    return fail(ParseFailureKind::NonCanonicalEncoding, std::move(msg), offset);
                }
                return fail(ParseFailureKind::NonCanonicalEncoding,
                            "raw control byte inside a string", offset);
            }
            value.push_back(ch);
            ++offset;
        }
        return fail(ParseFailureKind::NonCanonicalEncoding, "unterminated string", start);
    }
};

const std::vector<std::string_view>& schema_keys_for(JournalRecordType type) {
    static const std::vector<std::string_view> kRunStart = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "planned_move_count", "lock_id",
        "root_id", "anchor_id", "volume", "root_path", "undo_window_acknowledged",
        "supersedes_root_run_seq", "prev", "integrity"};
    static const std::vector<std::string_view> kIntent = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "move_id", "cat_id", "file_id",
        "file_ctime", "file_size", "file_mtime", "source_name", "dest_name", "dest_cat",
        "prev", "integrity"};
    static const std::vector<std::string_view> kResult = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "move_id", "cat_id", "file_id",
        "file_ctime", "file_size", "file_mtime", "source_name", "dest_name", "dest_cat",
        "outcome", "detail", "prev", "integrity"};
    static const std::vector<std::string_view> kRunEnd = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "planned_count", "moved_count",
        "conflict_count", "skipped_count", "failed_count", "prev", "integrity"};
    static const std::vector<std::string_view> kUndoStart = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "fwd_journal_digest",
        "undo_attempt_seq", "journal_id", "attempt_candidate_count", "prev", "integrity"};
    static const std::vector<std::string_view> kUndoIntent = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "fwd_journal_digest",
        "undo_attempt_seq", "attempt_move_seq", "undo_of_move", "cat_id", "file_id",
        "file_ctime", "file_size", "file_mtime", "source_name", "dest_name", "dest_cat",
        "prev", "integrity"};
    static const std::vector<std::string_view> kUndoResult = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "fwd_journal_digest",
        "undo_attempt_seq", "attempt_move_seq", "undo_of_move", "cat_id", "file_id",
        "file_ctime", "file_size", "file_mtime", "source_name", "dest_name", "dest_cat",
        "outcome", "detail", "prev", "integrity"};
    static const std::vector<std::string_view> kUndoEnd = {
        "v", "run", "dir", "type", "seq", "root_run_seq", "fwd_journal_digest",
        "undo_attempt_seq", "planned_count", "moved_count", "conflict_count",
        "skipped_count", "failed_count", "prev", "integrity"};
    switch (type) {
        case JournalRecordType::RunStart: return kRunStart;
        case JournalRecordType::Intent: return kIntent;
        case JournalRecordType::Result: return kResult;
        case JournalRecordType::RunEnd: return kRunEnd;
        case JournalRecordType::UndoStart: return kUndoStart;
        case JournalRecordType::UndoIntent: return kUndoIntent;
        case JournalRecordType::UndoResult: return kUndoResult;
        case JournalRecordType::UndoEnd: return kUndoEnd;
    }
    return kRunStart;  // unreachable
}

struct ParsedLine {
    bool ok = false;
    JournalRecord record{};
    std::string prev_hex;
    std::string integrity_hex;
    std::size_t integrity_comma_pos = 0;  // ',' position preceding the integrity member
};

// Reads the common head (v, run, dir, type) plus seq/root_run_seq and the
// undo mid fields. Returns the dispatched record type on success.
bool parse_head(LineParser& p, std::string& run, JournalDir& dir, JournalRecordType& type,
                unsigned long long& seq, unsigned long long& root_run_seq,
                std::string& fwd_digest, unsigned long long& undo_attempt_seq) {
    unsigned long long version = 0;
    if (!p.expect_key("v") || !p.read_u64_value(version, 999)) {
        return false;
    }
    if (version != 1) {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion, "unsupported schema version",
                      p.offset);
    }
    StringRules run_rules;
    run_rules.max_bytes = journal_run_max_bytes;
    if (!p.expect_key("run") || !p.read_string_value(run, run_rules, "run")) {
        return false;
    }
    StringRules dir_rules;
    dir_rules.max_bytes = kDirNameMax;
    dir_rules.forbid_escapes = true;
    dir_rules.forbid_controls = true;
    std::string dir_name;
    if (!p.expect_key("dir") || !p.read_string_value(dir_name, dir_rules, "dir")) {
        return false;
    }
    if (dir_name == "forward") {
        dir = JournalDir::Forward;
    } else if (dir_name == "undo") {
        dir = JournalDir::Undo;
    } else {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion, "unknown dir value", p.offset);
    }
    StringRules type_rules;
    type_rules.max_bytes = kTypeNameMax;
    type_rules.forbid_escapes = true;
    type_rules.forbid_controls = true;
    std::string type_name;
    if (!p.expect_key("type") || !p.read_string_value(type_name, type_rules, "type")) {
        return false;
    }
    std::optional<JournalRecordType> parsed;
    for (const JournalRecordType t :
         {JournalRecordType::RunStart, JournalRecordType::Intent, JournalRecordType::Result,
          JournalRecordType::RunEnd, JournalRecordType::UndoStart,
          JournalRecordType::UndoIntent, JournalRecordType::UndoResult,
          JournalRecordType::UndoEnd}) {
        if (journal_record_type_name(t) == type_name) {
            parsed = t;
            break;
        }
    }
    if (!parsed) {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion, "unknown record type", p.offset);
    }
    type = *parsed;
    const bool type_is_forward = type <= JournalRecordType::RunEnd;
    if ((dir == JournalDir::Forward) != type_is_forward) {
        return p.fail(ParseFailureKind::TransitionViolation,
                      "record type does not match the file direction", p.offset);
    }
    p.schema_keys = &schema_keys_for(type);
    if (!p.expect_key("seq") || !p.read_u64_value(seq, journal_seq_max)) {
        return false;
    }
    if (!p.expect_key("root_run_seq") || !p.read_u64_value(root_run_seq, kU64Max)) {
        return false;
    }
    if (!type_is_forward) {
        if (!p.expect_key("fwd_journal_digest") || !p.read_hex64_value(fwd_digest)) {
            return false;
        }
        if (!p.expect_key("undo_attempt_seq") || !p.read_u64_value(undo_attempt_seq, kU64Max)) {
            return false;
        }
    }
    return true;
}

bool finish_record(LineParser& p, ParsedLine& out, JournalRecord rec) {
    if (!p.expect_key("prev") || !p.read_hex64_value(out.prev_hex)) {
        return false;
    }
    out.integrity_comma_pos = p.offset;  // the ',' preceding the integrity member
    if (!p.expect_key("integrity") || !p.read_hex64_value(out.integrity_hex)) {
        return false;
    }
    if (!p.expect('}')) {
        return false;
    }
    if (!p.at_end()) {
        return p.fail(ParseFailureKind::NonCanonicalEncoding,
                      "trailing bytes after the record object", p.offset);
    }
    out.record = std::move(rec);
    out.ok = true;
    return true;
}

bool parse_record_schema(LineParser& p, ParsedLine& out) {
    if (!p.expect('{')) {
        return false;
    }
    std::string run;
    JournalDir dir = JournalDir::Forward;
    JournalRecordType type = JournalRecordType::RunStart;
    unsigned long long seq = 0;
    unsigned long long root_run_seq = 0;
    std::string fwd_digest;
    unsigned long long undo_attempt_seq = 0;
    if (!parse_head(p, run, dir, type, seq, root_run_seq, fwd_digest, undo_attempt_seq)) {
        return false;
    }

    switch (type) {
        case JournalRecordType::RunStart: {
            JournalRunStart r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            if (!p.expect_key("planned_move_count") ||
                !p.read_u64_value(r.planned_move_count, journal_move_limit)) {
                return false;
            }
            if (!p.expect_key("lock_id") ||
                !p.read_string_value(r.lock_id, identity_rules(), "lock_id")) {
                return false;
            }
            if (!p.expect_key("root_id") ||
                !p.read_string_value(r.root_id, identity_rules(), "root_id")) {
                return false;
            }
            if (!p.expect_key("anchor_id") ||
                !p.read_string_value(r.anchor_id, identity_rules(), "anchor_id")) {
                return false;
            }
            StringRules volume_rules;
            volume_rules.max_bytes = journal_volume_max_bytes;
            if (!p.expect_key("volume") ||
                !p.read_string_value(r.volume, volume_rules, "volume")) {
                return false;
            }
            StringRules path_rules;
            path_rules.max_bytes = journal_root_path_max_bytes;
            if (!p.expect_key("root_path") ||
                !p.read_string_value(r.root_path, path_rules, "root_path")) {
                return false;
            }
            if (p.try_expect_key("undo_window_acknowledged")) {
                // The pair serializes only as acknowledged:true + supersedes.
                if (p.line.compare(p.offset, 5, "false") == 0) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "explicit 'false' is not a canonical acknowledgement "
                                  "(the pair is serialized only as true, docs section 8)",
                                  p.offset);
                }
                if (p.line.compare(p.offset, 4, "true") == 0) {
                    p.offset += 4;
                } else {
                    return p.fail(ParseFailureKind::WrongValueType,
                                  "undo_window_acknowledged must be true", p.offset);
                }
                if (!p.expect_key("supersedes_root_run_seq") ||
                    !p.read_u64_value(r.supersedes_root_run_seq, kU64Max)) {
                    return false;
                }
                r.undo_window_acknowledged = true;
                r.has_ack = true;
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::Intent: {
            JournalIntent r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            if (!p.expect_key("move_id") || !p.read_u64_value(r.move_id, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("cat_id") ||
                !p.read_string_value(r.cat_id, identity_rules(), "cat_id")) {
                return false;
            }
            if (!p.expect_key("file_id") ||
                !p.read_string_value(r.file_id, identity_rules(), "file_id")) {
                return false;
            }
            if (!p.expect_key("file_ctime") || !p.read_u64_value(r.file_ctime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_size") || !p.read_u64_value(r.file_size, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_mtime") || !p.read_u64_value(r.file_mtime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("source_name") ||
                !p.read_string_value(r.source_name, name_rules(), "source_name")) {
                return false;
            }
            if (!p.expect_key("dest_name") ||
                !p.read_string_value(r.dest_name, name_rules(), "dest_name")) {
                return false;
            }
            StringRules cat_rules;
            cat_rules.max_bytes = kCategoryNameMax;
            cat_rules.forbid_escapes = true;
            cat_rules.forbid_controls = true;
            std::string cat_name;
            if (!p.expect_key("dest_cat") ||
                !p.read_string_value(cat_name, cat_rules, "dest_cat")) {
                return false;
            }
            const std::optional<Category> cat = category_from_name(cat_name);
            if (!cat) {
                return p.fail(ParseFailureKind::WrongValueType, "unknown category name",
                              p.offset);
            }
            r.dest_cat = *cat;
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::Result: {
            JournalResult r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            if (!p.expect_key("move_id") || !p.read_u64_value(r.move_id, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("cat_id") ||
                !p.read_string_value(r.cat_id, identity_rules(), "cat_id")) {
                return false;
            }
            if (!p.expect_key("file_id") ||
                !p.read_string_value(r.file_id, identity_rules(), "file_id")) {
                return false;
            }
            if (!p.expect_key("file_ctime") || !p.read_u64_value(r.file_ctime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_size") || !p.read_u64_value(r.file_size, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_mtime") || !p.read_u64_value(r.file_mtime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("source_name") ||
                !p.read_string_value(r.source_name, name_rules(), "source_name")) {
                return false;
            }
            if (!p.expect_key("dest_name") ||
                !p.read_string_value(r.dest_name, name_rules(), "dest_name")) {
                return false;
            }
            StringRules cat_rules;
            cat_rules.max_bytes = kCategoryNameMax;
            cat_rules.forbid_escapes = true;
            cat_rules.forbid_controls = true;
            std::string cat_name;
            if (!p.expect_key("dest_cat") ||
                !p.read_string_value(cat_name, cat_rules, "dest_cat")) {
                return false;
            }
            const std::optional<Category> cat = category_from_name(cat_name);
            if (!cat) {
                return p.fail(ParseFailureKind::WrongValueType, "unknown category name",
                              p.offset);
            }
            r.dest_cat = *cat;
            StringRules outcome_rules;
            outcome_rules.max_bytes = kOutcomeNameMax;
            outcome_rules.forbid_escapes = true;
            outcome_rules.forbid_controls = true;
            std::string outcome_name;
            if (!p.expect_key("outcome") ||
                !p.read_string_value(outcome_name, outcome_rules, "outcome")) {
                return false;
            }
            const std::optional<JournalOutcome> outcome =
                journal_outcome_from_name(outcome_name);
            if (!outcome) {
                return p.fail(ParseFailureKind::WrongValueType,
                              "outcome is outside the ratified vocabulary", p.offset);
            }
            if (*outcome == JournalOutcome::Restored) {
                return p.fail(ParseFailureKind::CorrelationAnomaly,
                              "RESULT cannot record the undo outcome 'restored'", p.offset);
            }
            r.outcome = *outcome;
            StringRules detail_rules;
            detail_rules.max_bytes = journal_detail_max_bytes;
            detail_rules.allow_empty = true;
            if (!p.expect_key("detail") ||
                !p.read_string_value(r.detail, detail_rules, "detail")) {
                return false;
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::RunEnd: {
            JournalRunEnd r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            if (!p.expect_key("planned_count") ||
                !p.read_u64_value(r.planned_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("moved_count") ||
                !p.read_u64_value(r.moved_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("conflict_count") ||
                !p.read_u64_value(r.conflict_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("skipped_count") ||
                !p.read_u64_value(r.skipped_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("failed_count") ||
                !p.read_u64_value(r.failed_count, journal_seq_max)) {
                return false;
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::UndoStart: {
            JournalUndoStart r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            r.fwd_journal_digest = fwd_digest;
            r.undo_attempt_seq = undo_attempt_seq;
            if (!p.expect_key("journal_id") ||
                !p.read_string_value(r.journal_id, identity_rules(), "journal_id")) {
                return false;
            }
            if (!p.expect_key("attempt_candidate_count") ||
                !p.read_u64_value(r.attempt_candidate_count, journal_move_limit)) {
                return false;
            }
            if (r.attempt_candidate_count == 0) {
                return p.fail(ParseFailureKind::CapExceeded,
                              "attempt_candidate_count must be at least 1", p.offset);
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::UndoIntent: {
            JournalUndoIntent r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            r.fwd_journal_digest = fwd_digest;
            r.undo_attempt_seq = undo_attempt_seq;
            if (!p.expect_key("attempt_move_seq") ||
                !p.read_u64_value(r.attempt_move_seq, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("undo_of_move") ||
                !p.read_u64_value(r.undo_of_move, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("cat_id") ||
                !p.read_string_value(r.cat_id, identity_rules(), "cat_id")) {
                return false;
            }
            if (!p.expect_key("file_id") ||
                !p.read_string_value(r.file_id, identity_rules(), "file_id")) {
                return false;
            }
            if (!p.expect_key("file_ctime") || !p.read_u64_value(r.file_ctime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_size") || !p.read_u64_value(r.file_size, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_mtime") || !p.read_u64_value(r.file_mtime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("source_name") ||
                !p.read_string_value(r.source_name, name_rules(), "source_name")) {
                return false;
            }
            if (!p.expect_key("dest_name") ||
                !p.read_string_value(r.dest_name, name_rules(), "dest_name")) {
                return false;
            }
            StringRules cat_rules;
            cat_rules.max_bytes = kCategoryNameMax;
            cat_rules.forbid_escapes = true;
            cat_rules.forbid_controls = true;
            std::string cat_name;
            if (!p.expect_key("dest_cat") ||
                !p.read_string_value(cat_name, cat_rules, "dest_cat")) {
                return false;
            }
            const std::optional<Category> cat = category_from_name(cat_name);
            if (!cat) {
                return p.fail(ParseFailureKind::WrongValueType, "unknown category name",
                              p.offset);
            }
            r.dest_cat = *cat;
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::UndoResult: {
            JournalUndoResult r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            r.fwd_journal_digest = fwd_digest;
            r.undo_attempt_seq = undo_attempt_seq;
            if (!p.expect_key("attempt_move_seq") ||
                !p.read_u64_value(r.attempt_move_seq, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("undo_of_move") ||
                !p.read_u64_value(r.undo_of_move, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("cat_id") ||
                !p.read_string_value(r.cat_id, identity_rules(), "cat_id")) {
                return false;
            }
            if (!p.expect_key("file_id") ||
                !p.read_string_value(r.file_id, identity_rules(), "file_id")) {
                return false;
            }
            if (!p.expect_key("file_ctime") || !p.read_u64_value(r.file_ctime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_size") || !p.read_u64_value(r.file_size, kU64Max)) {
                return false;
            }
            if (!p.expect_key("file_mtime") || !p.read_u64_value(r.file_mtime, kU64Max)) {
                return false;
            }
            if (!p.expect_key("source_name") ||
                !p.read_string_value(r.source_name, name_rules(), "source_name")) {
                return false;
            }
            if (!p.expect_key("dest_name") ||
                !p.read_string_value(r.dest_name, name_rules(), "dest_name")) {
                return false;
            }
            StringRules cat_rules;
            cat_rules.max_bytes = kCategoryNameMax;
            cat_rules.forbid_escapes = true;
            cat_rules.forbid_controls = true;
            std::string cat_name;
            if (!p.expect_key("dest_cat") ||
                !p.read_string_value(cat_name, cat_rules, "dest_cat")) {
                return false;
            }
            const std::optional<Category> cat = category_from_name(cat_name);
            if (!cat) {
                return p.fail(ParseFailureKind::WrongValueType, "unknown category name",
                              p.offset);
            }
            r.dest_cat = *cat;
            StringRules outcome_rules;
            outcome_rules.max_bytes = kOutcomeNameMax;
            outcome_rules.forbid_escapes = true;
            outcome_rules.forbid_controls = true;
            std::string outcome_name;
            if (!p.expect_key("outcome") ||
                !p.read_string_value(outcome_name, outcome_rules, "outcome")) {
                return false;
            }
            const std::optional<JournalOutcome> outcome =
                journal_outcome_from_name(outcome_name);
            if (!outcome) {
                return p.fail(ParseFailureKind::WrongValueType,
                              "outcome is outside the ratified vocabulary", p.offset);
            }
            r.outcome = *outcome;
            StringRules detail_rules;
            detail_rules.max_bytes = journal_detail_max_bytes;
            detail_rules.allow_empty = true;
            if (!p.expect_key("detail") ||
                !p.read_string_value(r.detail, detail_rules, "detail")) {
                return false;
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
        case JournalRecordType::UndoEnd: {
            JournalUndoEnd r;
            r.run = run;
            r.seq = seq;
            r.root_run_seq = root_run_seq;
            r.fwd_journal_digest = fwd_digest;
            r.undo_attempt_seq = undo_attempt_seq;
            if (!p.expect_key("planned_count") ||
                !p.read_u64_value(r.planned_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("moved_count") ||
                !p.read_u64_value(r.moved_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("conflict_count") ||
                !p.read_u64_value(r.conflict_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("skipped_count") ||
                !p.read_u64_value(r.skipped_count, journal_seq_max)) {
                return false;
            }
            if (!p.expect_key("failed_count") ||
                !p.read_u64_value(r.failed_count, journal_seq_max)) {
                return false;
            }
            return finish_record(p, out, JournalRecord{std::move(r)});
        }
    }
    return false;  // unreachable
}

// ---------------------------------------------------------------------------
// Whole-file validation: chain, transitions, continuity, tail.
// ---------------------------------------------------------------------------

struct ParseContext {
    std::string_view root_key;
    // Continuity head (captured from record 0).
    bool have_head = false;
    std::string run;
    unsigned long long root_run_seq = 0;
    JournalDir dir = JournalDir::Forward;
    std::string fwd_digest;
    unsigned long long undo_attempt_seq = 0;
    // Expected-record machine (docs section 6).
    enum class Step { WantStart, WantIntent, WantResult, WantEnd, Done };
    Step step = Step::WantStart;
    unsigned long long expected_move = 0;
    unsigned long long planned = 0;           // planned_move_count / attempt_candidate_count
    unsigned long long last_intent_undo_of = 0;
};

struct LineOutcome {
    bool ok = false;
    ParseFailure failure;
    JournalRecord record{};
    std::array<unsigned char, 32> digest{};
};

bool compute_integrity(ParseFailure& failure, std::string_view full_image,
                       std::size_t line_start, const ParsedLine& pl,
                       std::array<unsigned char, 32>& digest) {
    std::string body;
    body.reserve(pl.integrity_comma_pos + 1);
    body.append(full_image.substr(line_start, pl.integrity_comma_pos));
    body.push_back('}');
    const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(body.data()), body.size()));
    if (!h.ok) {
        failure.kind = ParseFailureKind::HashUnavailable;
        failure.offset = line_start;
        failure.detail = "SHA-256 provider failed: " + h.error;
        return false;
    }
    digest = h.digest;
    if (hex_of(h.digest) != pl.integrity_hex) {
        failure.kind = ParseFailureKind::ChainBreak;
        failure.offset = line_start;
        failure.detail = "integrity hash mismatch";
        return false;
    }
    return true;
}

bool check_prev(LineParser& p, ParseContext& ctx, std::size_t index,
                std::string_view prev_line_bytes, const ParsedLine& pl) {
    std::string expected_hex;
    if (index == 0) {
        unsigned long long genesis_root_run_seq = 0;
        std::string genesis_run;
        std::visit(
            [&](const auto& r) {
                genesis_root_run_seq = r.root_run_seq;
                genesis_run = r.run;
            },
            pl.record);
        const JournalHashResult genesis =
            journal_genesis_prev(ctx.root_key, genesis_root_run_seq, genesis_run);
        if (!genesis.ok) {
            return p.fail(ParseFailureKind::HashUnavailable,
                          "SHA-256 provider failed: " + genesis.error, 0);
        }
        expected_hex = genesis.hex;
    } else {
        const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(prev_line_bytes.data()),
            prev_line_bytes.size()));
        if (!h.ok) {
            return p.fail(ParseFailureKind::HashUnavailable,
                          "SHA-256 provider failed: " + h.error, 0);
        }
        expected_hex = hex_of(h.digest);
    }
    if (pl.prev_hex != expected_hex) {
        return p.fail(ParseFailureKind::ChainBreak, "prev hash does not chain to the "
                      "previous record", 0);
    }
    return true;
}

bool check_transition_and_continuity(LineParser& p, ParseContext& ctx, std::size_t index,
                                     const JournalRecord& rec) {
    // seq must equal the record index (0-based, gap-free).
    std::size_t seq = 0;
    std::visit([&](const auto& r) { seq = static_cast<std::size_t>(r.seq); }, rec);
    if (seq != index) {
        return p.fail(ParseFailureKind::TransitionViolation,
                      "seq does not match the record position", 0);
    }
    // The record's variant family IS its direction: parse_head has already
    // rejected dir/type mismatches. The context's dir is only learned from
    // record 0 in the continuity block below, so deriving `forward` from it
    // here would misjudge every undo file at its first record.
    const bool forward = rec.index() <= 3;
    switch (ctx.step) {
        case ParseContext::Step::WantStart: {
            if (forward) {
                const JournalRunStart* r = std::get_if<JournalRunStart>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "the first record must be RUN-START", 0);
                }
                ctx.planned = r->planned_move_count;
            } else {
                const JournalUndoStart* r = std::get_if<JournalUndoStart>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "the first record must be UNDO-START", 0);
                }
                ctx.planned = r->attempt_candidate_count;
            }
            ctx.step = ctx.planned > 0 ? ParseContext::Step::WantIntent
                                       : ParseContext::Step::WantEnd;
            ctx.expected_move = 0;
            break;
        }
        case ParseContext::Step::WantIntent: {
            if (forward) {
                const JournalIntent* r = std::get_if<JournalIntent>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "expected INTENT for the next move", 0);
                }
                if (r->move_id != ctx.expected_move) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "INTENT move_id is not the expected next move", 0);
                }
            } else {
                const JournalUndoIntent* r = std::get_if<JournalUndoIntent>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "expected UNDO-INTENT for the next candidate", 0);
                }
                if (r->attempt_move_seq != ctx.expected_move) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "UNDO-INTENT attempt_move_seq is not the expected next "
                                  "candidate", 0);
                }
                ctx.last_intent_undo_of = r->undo_of_move;
            }
            ctx.step = ParseContext::Step::WantResult;
            break;
        }
        case ParseContext::Step::WantResult: {
            if (forward) {
                const JournalResult* r = std::get_if<JournalResult>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "expected RESULT for the open INTENT", 0);
                }
                if (r->move_id != ctx.expected_move) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "RESULT move_id does not match the open INTENT", 0);
                }
            } else {
                const JournalUndoResult* r = std::get_if<JournalUndoResult>(&rec);
                if (r == nullptr) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "expected UNDO-RESULT for the open UNDO-INTENT", 0);
                }
                if (r->attempt_move_seq != ctx.expected_move) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "UNDO-RESULT attempt_move_seq does not match the open "
                                  "UNDO-INTENT", 0);
                }
                if (r->undo_of_move != ctx.last_intent_undo_of) {
                    return p.fail(ParseFailureKind::CorrelationAnomaly,
                                  "UNDO-RESULT undo_of_move does not match the open "
                                  "UNDO-INTENT", 0);
                }
            }
            ++ctx.expected_move;
            ctx.step = ctx.expected_move < ctx.planned ? ParseContext::Step::WantIntent
                                                       : ParseContext::Step::WantEnd;
            break;
        }
        case ParseContext::Step::WantEnd: {
            if (forward) {
                if (!std::holds_alternative<JournalRunEnd>(rec)) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "all planned moves are final; RUN-END is the only "
                                  "permitted record", 0);
                }
            } else {
                if (!std::holds_alternative<JournalUndoEnd>(rec)) {
                    return p.fail(ParseFailureKind::TransitionViolation,
                                  "all candidates are final; UNDO-END is the only "
                                  "permitted record", 0);
                }
            }
            ctx.step = ParseContext::Step::Done;
            break;
        }
        case ParseContext::Step::Done:
            return p.fail(ParseFailureKind::TransitionViolation,
                          "record after the terminal END record", 0);
    }
    // Continuity (docs section 5): run, root_run_seq, dir, and the per-segment
    // fields are constant within one file.
    std::string run;
    unsigned long long rrs = 0;
    std::visit(
        [&](const auto& r) {
            run = r.run;
            rrs = r.root_run_seq;
        },
        rec);
    if (ctx.have_head) {
        if (run != ctx.run || rrs != ctx.root_run_seq) {
            return p.fail(ParseFailureKind::ContinuityMismatch,
                          "run/root_run_seq discontinuity within the file", 0);
        }
        if (!forward) {
            const std::string* digest = nullptr;
            unsigned long long uas = 0;
            if (const auto* r = std::get_if<JournalUndoStart>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoIntent>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoResult>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoEnd>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            }
            if (digest == nullptr || *digest != ctx.fwd_digest || uas != ctx.undo_attempt_seq) {
                return p.fail(ParseFailureKind::ContinuityMismatch,
                              "fwd_journal_digest/undo_attempt_seq discontinuity within "
                              "the segment", 0);
            }
        }
    } else {
        ctx.have_head = true;
        ctx.run = run;
        ctx.root_run_seq = rrs;
        ctx.dir = forward ? JournalDir::Forward : JournalDir::Undo;
        // dir continuity is implied by the per-record family check, but the
        // head value is recorded for the tail attempt's snapshot.
        if (!forward) {
            const std::string* digest = nullptr;
            unsigned long long uas = 0;
            if (const auto* r = std::get_if<JournalUndoStart>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoIntent>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoResult>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            } else if (const auto* r = std::get_if<JournalUndoEnd>(&rec)) {
                digest = &r->fwd_journal_digest;
                uas = r->undo_attempt_seq;
            }
            if (digest != nullptr) {
                ctx.fwd_digest = *digest;
                ctx.undo_attempt_seq = uas;
            }
        }
    }
    return true;
}

LineOutcome validate_line(std::string_view line_bytes, std::size_t line_start,
                          std::string_view full_image, std::size_t index,
                          ParseContext& ctx, std::string_view prev_line_bytes) {
    LineOutcome out;
    LineParser p;
    p.line = line_bytes;
    p.line_start = line_start;
    ParsedLine pl;
    if (!parse_record_schema(p, pl)) {
        out.failure = p.failure_at_current();
        return out;
    }
    if (!compute_integrity(out.failure, full_image, line_start, pl, out.digest)) {
        return out;
    }
    // On any failure below, the caller aborts the whole parse, so a
    // half-advanced context is never observed. The tail attempt passes a
    // snapshot so a torn tail cannot advance the committed machine.
    if (!check_prev(p, ctx, index, prev_line_bytes, pl)) {
        out.failure = p.failure_at_current();
        return out;
    }
    if (!check_transition_and_continuity(p, ctx, index, pl.record)) {
        out.failure = p.failure_at_current();
        return out;
    }
    out.record = std::move(pl.record);
    out.ok = true;
    return out;
}

// Best-effort torn-tail type hint (R20, expected-record-aware classification).
// The hint is produced only when the tail's canonical prefix structurally
// reaches the `type` value and exactly one known type prefix-matches, so
// content bytes (e.g. a detail string) can never fake a hint.
std::optional<JournalRecordType> scan_tail_type_hint(std::string_view tail) {
    static constexpr std::string_view kRunPrefix = "{\"v\":1,\"run\":\"";
    if (!tail.starts_with(kRunPrefix)) {
        return std::nullopt;
    }
    std::size_t pos = kRunPrefix.size();
    auto scan_string = [&](std::string_view closer) -> bool {
        while (pos < tail.size()) {
            const char c = tail[pos];
            if (c == '\\') {
                pos += 2;
                continue;
            }
            if (c == '"') {
                break;
            }
            ++pos;
        }
        if (pos >= tail.size()) {
            return false;
        }
        ++pos;  // closing quote
        if (!tail.substr(pos).starts_with(closer)) {
            return false;
        }
        pos += closer.size();
        return true;
    };
    if (!scan_string(",\"dir\":\"")) {
        return std::nullopt;
    }
    if (!scan_string(",\"type\":\"")) {
        return std::nullopt;
    }
    std::string_view observed = tail.substr(pos);
    const std::size_t quote = observed.find('"');
    if (quote != std::string_view::npos) {
        observed = observed.substr(0, quote);
    }
    std::optional<JournalRecordType> match;
    for (const JournalRecordType t :
         {JournalRecordType::RunStart, JournalRecordType::Intent, JournalRecordType::Result,
          JournalRecordType::RunEnd, JournalRecordType::UndoStart,
          JournalRecordType::UndoIntent, JournalRecordType::UndoResult,
          JournalRecordType::UndoEnd}) {
        const std::string_view name = journal_record_type_name(t);
        if (observed.size() <= name.size() && name.substr(0, observed.size()) == observed) {
            if (match) {
                return std::nullopt;  // ambiguous prefix
            }
            match = t;
        }
    }
    return match;
}

} // namespace

// ---------------------------------------------------------------------------
// Public name tables.
// ---------------------------------------------------------------------------

std::string_view journal_record_type_name(JournalRecordType type) {
    static constexpr std::string_view kNames[] = {
        "RUN-START", "INTENT", "RESULT", "RUN-END",
        "UNDO-START", "UNDO-INTENT", "UNDO-RESULT", "UNDO-END"};
    const auto index = static_cast<std::size_t>(type);
    if (index >= std::size(kNames)) {
        return {};
    }
    return kNames[index];
}

std::string_view journal_outcome_name(JournalOutcome outcome) {
    static constexpr std::string_view kNames[] = {
        "Moved", "Conflict", "SourceMissing", "SourceReplaced", "SourceUnsafe", "Locked",
        "AccessDenied", "DestinationUnsafe", "DestinationChanged", "CrossVolume",
        "DestinationInvalid", "SourceInvalid", "Failed", "restored"};
    const auto index = static_cast<std::size_t>(outcome);
    if (index >= std::size(kNames)) {
        return {};
    }
    return kNames[index];
}

std::optional<JournalOutcome> journal_outcome_from_name(std::string_view name) {
    for (const JournalOutcome o : {JournalOutcome::Moved, JournalOutcome::Conflict,
                                   JournalOutcome::SourceMissing, JournalOutcome::SourceReplaced,
                                   JournalOutcome::SourceUnsafe, JournalOutcome::Locked,
                                   JournalOutcome::AccessDenied, JournalOutcome::DestinationUnsafe,
                                   JournalOutcome::DestinationChanged, JournalOutcome::CrossVolume,
                                   JournalOutcome::DestinationInvalid,
                                   JournalOutcome::SourceInvalid, JournalOutcome::Failed,
                                   JournalOutcome::Restored}) {
        if (journal_outcome_name(o) == name) {
            return o;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Serialization.
// ---------------------------------------------------------------------------

std::string journal_hex(std::span<const unsigned char> bytes) {
    return hex_of(bytes);
}

JournalHashResult journal_genesis_prev(std::string_view root_key,
                                       unsigned long long root_run_seq, std::string_view run) {
    JournalHashResult result;
    std::string seed;
    seed.reserve(root_key.size() + 20 + run.size());
    seed.append(root_key);
    append_u64(seed, root_run_seq);
    seed.append(run);
    const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(seed.data()), seed.size()));
    if (!h.ok) {
        result.error = "SHA-256 provider failed: " + h.error;
        return result;
    }
    result.ok = true;
    result.hex = hex_of(h.digest);
    return result;
}

namespace {

// Appends the `integrity` member and the terminating LF. `core` holds the
// canonical record bytes that the hash covers: everything before the
// `integrity` member (including `prev` when the record carries one), with
// the closing brace re-appended - exactly the bytes the parser reconstructs
// when it verifies the hash. The terminating LF is never hashed.
bool assemble_record_line(std::string& core, std::string& out, std::string& error) {
    const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(core.data()), core.size()));
    if (!h.ok) {
        error = "SHA-256 provider failed: " + h.error;
        return false;
    }
    out.reserve(core.size() + journal_hex_chars + 16);
    out = std::move(core);
    out.pop_back();  // drop the closing brace; the integrity member follows
    out += ",\"integrity\":\"";
    out += hex_of(h.digest);
    out += "\"}\n";
    return true;
}

} // namespace

JournalSerializeResult serialize_journal_record(const JournalRecord& record,
                                                std::string_view prev_hex) {
    JournalSerializeResult result;
    std::string error;
    if (!check_hex64(error, prev_hex, "prev")) {
        result.error = std::move(error);
        return result;
    }
    const Body body = std::visit([](const auto& r) { return build_body(r); }, record);
    if (!body.ok) {
        result.error = body.error;
        return result;
    }
    // Insert `prev` as the second-to-last member: the hashed core is the
    // body with `prev` before the closing brace, `integrity` excluded.
    std::string core;
    core.reserve(body.bytes.size() + journal_hex_chars + 12);
    core.append(body.bytes, 0, body.bytes.size() - 1);  // drop the closing brace
    core += ",\"prev\":\"";
    core += prev_hex;
    core += "\"}";
    std::string line;
    if (!assemble_record_line(core, line, result.error)) {
        return result;
    }
    if (line.size() > journal_max_record_bytes) {
        result.error = "serialized record exceeds the 8 KiB cap";
        return result;
    }
    result.ok = true;
    result.canonical = std::move(line);
    return result;
}

JournalSerializeResult serialize_journal_chain(std::span<const JournalRecord> records,
                                               std::string_view root_key) {
    JournalSerializeResult result;
    if (records.empty()) {
        result.ok = true;  // the abandoned-initialization file state
        return result;
    }
    unsigned long long root_run_seq = 0;
    std::string run;
    bool head_ok = false;
    if (const auto* r = std::get_if<JournalRunStart>(&records[0])) {
        root_run_seq = r->root_run_seq;
        run = r->run;
        head_ok = true;
    } else if (const auto* r = std::get_if<JournalUndoStart>(&records[0])) {
        root_run_seq = r->root_run_seq;
        run = r->run;
        head_ok = true;
    }
    if (!head_ok) {
        result.error = "a journal chain must begin with RUN-START or UNDO-START";
        return result;
    }
    const JournalHashResult genesis = journal_genesis_prev(root_key, root_run_seq, run);
    if (!genesis.ok) {
        result.error = genesis.error;
        return result;
    }
    std::string prev = genesis.hex;
    for (const JournalRecord& rec : records) {
        const JournalSerializeResult line = serialize_journal_record(rec, prev);
        if (!line.ok) {
            result.error = line.error;
            return result;
        }
        // prev of the next record = SHA-256 over this record's full canonical
        // bytes including its integrity member, excluding the LF.
        std::string_view line_view(line.canonical);
        line_view.remove_suffix(1);  // LF
        const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(line_view.data()), line_view.size()));
        if (!h.ok) {
            result.error = "SHA-256 provider failed: " + h.error;
            return result;
        }
        prev = hex_of(h.digest);
        result.canonical += line.canonical;
    }
    if (result.canonical.size() > journal_max_file_bytes) {
        result.error = "serialized chain exceeds the 16 MiB per-file limit";
        return result;
    }
    result.ok = true;
    return result;
}

JournalSerializeResult serialize_anchor_record(const AnchorRecord& record) {
    JournalSerializeResult result;
    const Body body = build_body(record);
    if (!body.ok) {
        result.error = body.error;
        return result;
    }
    // Anchor records carry no `prev`: the hashed core is the body as built.
    std::string core = body.bytes;
    std::string line;
    if (!assemble_record_line(core, line, result.error)) {
        return result;
    }
    if (line.size() > 450) {
        result.error = "anchor record exceeds the 450-byte ceiling (docs section 5)";
        return result;
    }
    result.ok = true;
    result.canonical = std::move(line);
    return result;
}

// ---------------------------------------------------------------------------
// Whole-file parsing.
// ---------------------------------------------------------------------------

namespace {

struct SplitImage {
    std::vector<std::pair<std::size_t, std::size_t>> lines;  // [start, length), LF excluded
    bool has_tail = false;
    std::size_t tail_start = 0;
};

SplitImage split_lines(std::string_view image) {
    SplitImage split;
    if (image.empty()) {
        return split;
    }
    if (image.back() == '\n') {
        std::size_t pos = 0;
        while (pos < image.size()) {
            const std::size_t nl = image.find('\n', pos);
            split.lines.emplace_back(pos, nl - pos);
            pos = nl + 1;
        }
        return split;
    }
    const std::size_t last_nl = image.rfind('\n');
    std::size_t pos = 0;
    while (last_nl != std::string_view::npos && pos <= last_nl) {
        const std::size_t nl = image.find('\n', pos);
        split.lines.emplace_back(pos, nl - pos);
        pos = nl + 1;
    }
    split.has_tail = true;
    split.tail_start = last_nl == std::string_view::npos ? 0 : last_nl + 1;
    return split;
}

} // namespace

ParsedJournalFile parse_journal_file(std::span<const unsigned char> bytes,
                                     std::string_view root_key) {
    ParsedJournalFile out;
    out.total_bytes = bytes.size();
    const std::string_view image(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (bytes.size() > journal_max_file_bytes) {
        out.failure.kind = ParseFailureKind::OversizeFile;
        out.failure.offset = 0;
        out.failure.detail = "input exceeds the 16 MiB per-file limit";
        return out;
    }
    const SplitImage split = split_lines(image);

    ParseContext ctx;
    ctx.root_key = root_key;
    std::string_view prev_line_bytes;
    std::size_t index = 0;
    for (const auto& [start, length] : split.lines) {
        if (length == 0) {
            out.failure.kind = ParseFailureKind::NonCanonicalEncoding;
            out.failure.offset = start;
            out.failure.detail = "empty line is not a canonical record";
            return out;
        }
        // The ratified caps cover the stored record including its
        // terminating LF (docs section 5); `length` excludes it.
        if (length + 1 > journal_max_record_bytes) {
            out.failure.kind = ParseFailureKind::CapExceeded;
            out.failure.offset = start;
            out.failure.detail = "record exceeds the 8 KiB cap";
            return out;
        }
        const LineOutcome lo = validate_line(image.substr(start, length), start, image, index,
                                             ctx, prev_line_bytes);
        if (!lo.ok) {
            out.failure = lo.failure;
            return out;  // valid stays false
        }
        out.records.push_back(lo.record);
        out.record_digests.push_back(lo.digest);
        out.record_end.push_back(start + length + 1);  // include the LF
        prev_line_bytes = image.substr(start, length);
        ++index;
    }
    if (split.has_tail) {
        const std::string_view tail = image.substr(split.tail_start);
        if (!tail.empty()) {
            // The tail is the record's canonical bytes minus the terminating
            // LF; the cap covers the LF too (docs section 5).
            if (tail.size() + 1 > journal_max_record_bytes) {
                out.failure.kind = ParseFailureKind::CapExceeded;
                out.failure.offset = split.tail_start;
                out.failure.detail = "record exceeds the 8 KiB cap";
                return out;
            }
            ParseContext tail_ctx = ctx;
            const LineOutcome lo = validate_line(tail, split.tail_start, image, index,
                                                 tail_ctx, prev_line_bytes);
            if (lo.ok) {
                out.records.push_back(lo.record);
                out.record_digests.push_back(lo.digest);
                out.record_end.push_back(out.total_bytes);
                out.unterminated_final_record = true;
                out.tail = JournalTailKind::CleanEof;
            } else {
                out.tail = JournalTailKind::Torn;
                out.tail_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(split.tail_start),
                                      bytes.end());
                out.tail_type_hint = scan_tail_type_hint(tail);
            }
        }
    }
    out.valid = true;
    return out;
}

namespace {

struct AnchorLine {
    bool ok = false;
    AnchorRecord record{};
    std::string integrity_hex;
    std::size_t integrity_comma_pos = 0;
};

bool parse_anchor_schema(LineParser& p, AnchorLine& out) {
    if (!p.expect('{')) {
        return false;
    }
    unsigned long long version = 0;
    if (!p.expect_key("v") || !p.read_u64_value(version, 999)) {
        return false;
    }
    if (version != 1) {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion, "unsupported schema version",
                      p.offset);
    }
    StringRules exact_rules;
    exact_rules.max_bytes = kTypeNameMax;
    exact_rules.forbid_escapes = true;
    exact_rules.forbid_controls = true;
    std::string kind_name;
    if (!p.expect_key("kind") || !p.read_string_value(kind_name, exact_rules, "kind")) {
        return false;
    }
    if (kind_name != "anchor") {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion, "unknown kind value", p.offset);
    }
    AnchorRecord r;
    if (!p.expect_key("root_run_seq") || !p.read_u64_value(r.root_run_seq, kU64Max)) {
        return false;
    }
    std::string file_kind;
    if (!p.expect_key("file_kind") ||
        !p.read_string_value(file_kind, exact_rules, "file_kind")) {
        return false;
    }
    if (file_kind != "journal") {
        return p.fail(ParseFailureKind::UnknownTypeOrVersion,
                      "unknown file_kind value (attempt segments are unanchored in "
                      "Phase 7 [G9])", p.offset);
    }
    if (!p.expect_key("journal_id") ||
        !p.read_string_value(r.journal_id, identity_rules(), "journal_id")) {
        return false;
    }
    if (!p.expect_key("anchor_seq") || !p.read_u64_value(r.anchor_seq, journal_seq_max)) {
        return false;
    }
    if (!p.expect_key("covered_seq") || !p.read_u64_value(r.covered_seq, journal_seq_max)) {
        return false;
    }
    if (!p.expect_key("byte_len") || !p.read_u64_value(r.byte_len, kU64Max)) {
        return false;
    }
    if (!p.expect_key("last_record_hash") || !p.read_hex64_value(r.last_record_hash)) {
        return false;
    }
    out.integrity_comma_pos = p.offset;
    if (!p.expect_key("integrity") || !p.read_hex64_value(out.integrity_hex)) {
        return false;
    }
    if (!p.expect('}')) {
        return false;
    }
    if (!p.at_end()) {
        return p.fail(ParseFailureKind::NonCanonicalEncoding,
                      "trailing bytes after the anchor record object", p.offset);
    }
    out.record = std::move(r);
    out.ok = true;
    return true;
}

bool validate_anchor_line(std::string_view line_bytes, std::size_t line_start,
                          std::string_view full_image, std::size_t index, AnchorLine& out,
                          ParseFailure& failure, bool& have_head, unsigned long long& head_rrs,
                          std::string& head_journal_id, unsigned long long& prev_byte_len) {
    LineParser p;
    p.line = line_bytes;
    p.line_start = line_start;
    if (!parse_anchor_schema(p, out)) {
        failure = p.failure_at_current();
        return false;
    }
    std::string body;
    body.append(full_image.substr(line_start, out.integrity_comma_pos));
    body.push_back('}');
    const platform::Sha256Result h = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(body.data()), body.size()));
    if (!h.ok) {
        failure.kind = ParseFailureKind::HashUnavailable;
        failure.offset = line_start;
        failure.detail = "SHA-256 provider failed: " + h.error;
        return false;
    }
    if (hex_of(h.digest) != out.integrity_hex) {
        failure.kind = ParseFailureKind::ChainBreak;
        failure.offset = line_start;
        failure.detail = "anchor record integrity hash mismatch";
        return false;
    }
    // Ordering invariants: anchor_seq gap-free from 0, covered_seq contiguous
    // ascending from 0 (the genesis record covers RUN-START), byte_len
    // strictly increasing, per-file fields constant.
    if (out.record.anchor_seq != index || out.record.covered_seq != index) {
        failure.kind = ParseFailureKind::TransitionViolation;
        failure.offset = line_start;
        failure.detail = "anchor_seq/covered_seq are not contiguous from 0";
        return false;
    }
    if (index == 0) {
        if (out.record.byte_len == 0) {
            failure.kind = ParseFailureKind::CorrelationAnomaly;
            failure.offset = line_start;
            failure.detail = "the genesis anchor record must cover at least one byte";
            return false;
        }
    } else {
        if (out.record.byte_len <= prev_byte_len) {
            failure.kind = ParseFailureKind::TransitionViolation;
            failure.offset = line_start;
            failure.detail = "byte_len must strictly increase";
            return false;
        }
        if (!have_head || out.record.root_run_seq != head_rrs ||
            out.record.journal_id != head_journal_id) {
            failure.kind = ParseFailureKind::ContinuityMismatch;
            failure.offset = line_start;
            failure.detail = "root_run_seq/journal_id discontinuity within the anchor file";
            return false;
        }
    }
    have_head = true;
    head_rrs = out.record.root_run_seq;
    head_journal_id = out.record.journal_id;
    prev_byte_len = out.record.byte_len;
    return true;
}

} // namespace

ParsedAnchorFile parse_anchor_file(std::span<const unsigned char> bytes) {
    ParsedAnchorFile out;
    out.total_bytes = bytes.size();
    const std::string_view image(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (bytes.size() > journal_max_file_bytes) {
        out.failure.kind = ParseFailureKind::OversizeFile;
        out.failure.offset = 0;
        out.failure.detail = "input exceeds the 16 MiB per-file limit";
        return out;
    }
    const SplitImage split = split_lines(image);
    bool have_head = false;
    unsigned long long head_rrs = 0;
    std::string head_journal_id;
    unsigned long long prev_byte_len = 0;
    std::size_t index = 0;
    for (const auto& [start, length] : split.lines) {
        if (length == 0) {
            out.failure.kind = ParseFailureKind::NonCanonicalEncoding;
            out.failure.offset = start;
            out.failure.detail = "empty line is not a canonical record";
            return out;
        }
        // The ratified caps cover the stored record including its
        // terminating LF (docs section 5); `length` excludes it.
        if (length + 1 > 450) {
            out.failure.kind = ParseFailureKind::CapExceeded;
            out.failure.offset = start;
            out.failure.detail = "anchor record exceeds the 450-byte ceiling";
            return out;
        }
        AnchorLine al;
        if (!validate_anchor_line(image.substr(start, length), start, image, index, al,
                                  out.failure, have_head, head_rrs, head_journal_id,
                                  prev_byte_len)) {
            return out;
        }
        out.records.push_back(al.record);
        out.record_end.push_back(start + length + 1);
        ++index;
    }
    if (split.has_tail) {
        const std::string_view tail = image.substr(split.tail_start);
        if (!tail.empty()) {
            // The tail is the anchor record's canonical bytes minus the
            // terminating LF; the ceiling covers the LF too (docs section 5).
            if (tail.size() + 1 > 450) {
                out.failure.kind = ParseFailureKind::CapExceeded;
                out.failure.offset = split.tail_start;
                out.failure.detail = "anchor record exceeds the 450-byte ceiling";
                return out;
            }
            AnchorLine al;
            ParseFailure failure;
            if (validate_anchor_line(tail, split.tail_start, image, index, al, failure,
                                     have_head, head_rrs, head_journal_id, prev_byte_len)) {
                out.records.push_back(al.record);
                out.record_end.push_back(out.total_bytes);
                out.unterminated_final_record = true;
                out.tail = JournalTailKind::CleanEof;
            } else {
                out.tail = JournalTailKind::Torn;
                out.tail_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(split.tail_start),
                                      bytes.end());
            }
        }
    }
    out.valid = true;
    return out;
}

} // namespace folderpilot
