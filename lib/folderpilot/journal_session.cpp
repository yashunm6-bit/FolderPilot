// FolderPilot - Phase 7.2 write-ahead journal session implementation.
//
// Storage classification, sequence allocation, capacity admission, leaf
// creation, and the write-ahead record chain with ratified anchor ordering
// (docs/PHASE7-DESIGN.md §20). Every failure is a structured, classified
// stop - nothing is retried, rewritten or silently continued.

#include "folderpilot/journal_session.hpp"

#include "sha256.hpp"

#include <cstdio>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace folderpilot {

namespace {

using JournalSessionResultF = JournalSessionResult;

// The 20-digit sequence field cap: names parse into the full unsigned
// 64-bit range; anything beyond is malformed (unrecognized).
constexpr unsigned long long kMaxStorageSequence = std::numeric_limits<unsigned long long>::max();

// Recognized storage-entry kinds.
enum class StorageEntryKind { Lock, Journal, Anchor, Undo };

struct StorageEntryClass {
    StorageEntryKind kind = StorageEntryKind::Lock;
    unsigned long long r = 0;   // root_run_seq component
    unsigned long long a = 0;   // undo_attempt_seq component (undo names)
};

// Parses exactly 20 ASCII digits into an unsigned long long (the field cap
// spans the full uint64 range; overflow cannot occur for 20 digits that fit,
// and values beyond uint64 are malformed by construction).
bool parse20(std::wstring_view digits, unsigned long long& out) {
    if (digits.size() != 20) {
        return false;
    }
    unsigned long long value = 0;
    for (const wchar_t c : digits) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        const auto digit = static_cast<unsigned long long>(c - L'0');
        if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) {
            return false; // beyond the uint64 sequence space: malformed
        }
        value = value * 10ULL + digit;
    }
    out = value;
    return true;
}

// Classifies a storage-directory entry name. nullopt = unrecognized (the
// caller refuses). Malformed recognized-looking names (wrong digit count,
// non-digits) are unrecognized too - they could hide a sequence, so the
// storage state is not safely classifiable.
std::optional<StorageEntryClass> classify_storage_name(std::wstring_view n) {
    if (n == L"mutation.lock") {
        return StorageEntryClass{StorageEntryKind::Lock, 0, 0};
    }
    // journal-<20d>.jsonl  = 8 + 20 + 6 = 34 wide chars
    if (n.size() == 34 && n.starts_with(L"journal-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        if (!parse20(n.substr(8, 20), r)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Journal, r, 0};
    }
    // anchor-<20d>.jsonl  = 7 + 20 + 6 = 33 wide chars
    if (n.size() == 33 && n.starts_with(L"anchor-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        if (!parse20(n.substr(7, 20), r)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Anchor, r, 0};
    }
    // undo-<20d>-<20d>.jsonl = 5 + 20 + 1 + 20 + 6 = 52 wide chars
    if (n.size() == 52 && n.starts_with(L"undo-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        unsigned long long a = 0;
        if (!parse20(n.substr(5, 20), r) || n[25] != L'-' ||
            !parse20(n.substr(26, 20), a)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Undo, r, a};
    }
    return std::nullopt;
}

JournalSessionResult map_lock_failure(const MutationLock& lock) {
    JournalSessionResult out;
    if (lock.failure == StorageFailure::SharingViolation) {
        out.failure = JournalSessionFailure::LockHeld;
        out.error = "another FolderPilot operation holds the mutation lock: " + lock.error;
    } else {
        out.failure = JournalSessionFailure::LockFailure;
        out.error = "mutation lock acquisition failed: " + lock.error;
        out.native_code = lock.native_code;
    }
    return out;
}

JournalSessionResult session_fail(JournalSessionFailure failure, std::string error,
                                  unsigned long native_code = 0) {
    JournalSessionResult out;
    out.failure = failure;
    out.error = std::move(error);
    out.native_code = native_code;
    return out;
}

} // namespace

JournalSession::JournalSession(StorageRoot root, MutationLock lock)
    : root_(std::move(root)), lock_(std::move(lock)) {}

JournalSessionOpenResult JournalSession::open_for_run(
    std::string_view storage_base_path,
    const platform::RootDirectory& organized_root,
    unsigned long long planned_move_count,
    bool undo_window_acknowledged,
    unsigned long long supersedes_root_run_seq) {
    JournalSessionOpenResult out;

    if (!organized_root.valid() || organized_root.volume.volume_guid_path.empty()) {
        out.failure = JournalSessionFailure::StorageInvalid;
        out.error = "journal session requires a validated organized root with "
                    "volume identity";
        return out;
    }

    // 1. Storage root (handle-anchored chain, ACL-validated [G4]).
    auto root = open_storage_root(storage_base_path, organized_root);
    if (!root.ok) {
        // Storage-validity failures are safety refusals (§20.8: exit 2).
        out.failure = JournalSessionFailure::StorageClassification;
        out.error = "storage root: " + root.error;
        out.native_code = root.native_code;
        return out;
    }

    // 2. Exclusive mutation lock.
    auto lock = acquire_mutation_lock(root);
    if (!lock.ok) {
        out.failure = map_lock_failure(lock).failure;
        out.error = map_lock_failure(lock).error;
        out.native_code = map_lock_failure(lock).native_code;
        return out;
    }

    // 3. Classify the storage directory and allocate the next sequence.
    unsigned long long max_seq = 0;
    bool saw_any = false;
    bool unrecognized = false;
    std::string unrecognized_name;
    std::vector<unsigned long long> journal_seqs;
    std::vector<unsigned long long> anchor_seqs;
    const auto enumerated = platform::enumerate_immediate_children(
        root.root_key_dir.get(), 64 * 1024,
        [&](const platform::RawDirEntry& entry) {
            if (entry.name == L"." || entry.name == L"..") {
                return; // quota parity: prefix records are not storage entries
            }
            saw_any = true;
            const auto classified = classify_storage_name(entry.name);
            if (!classified) {
                if (unrecognized_name.empty()) {
                    unrecognized_name = std::string(entry.name.begin(), entry.name.end());
                }
                unrecognized = true;
                return;
            }
            if (classified->kind == StorageEntryKind::Journal) {
                journal_seqs.push_back(classified->r);
                if (classified->r > max_seq) {
                    max_seq = classified->r;
                }
            } else if (classified->kind == StorageEntryKind::Undo) {
                if (classified->r > max_seq) {
                    max_seq = classified->r;
                }
            } else if (classified->kind == StorageEntryKind::Anchor) {
                anchor_seqs.push_back(classified->r);
            }
        });
    if (!enumerated.ok) {
        out.failure = JournalSessionFailure::StorageClassification;
        out.error = "storage directory enumeration failed: " + enumerated.error;
        out.native_code = enumerated.native_code;
        return out;
    }
    if (unrecognized) {
        out.failure = JournalSessionFailure::StorageClassification;
        out.error = "unrecognized storage entry under <root-key>: " + unrecognized_name +
                    "; the storage state cannot be classified safely";
        return out;
    }
    // Journal/anchor mismatch: an anchor whose journal is absent is a state
    // the protocol never produces (the journal is always created first) -
    // refuse rather than choose (ADDENDUM §2).
    for (const unsigned long long a : anchor_seqs) {
        bool has_journal = false;
        for (const unsigned long long j : journal_seqs) {
            if (j == a) {
                has_journal = true;
                break;
            }
        }
        if (!has_journal) {
            out.failure = JournalSessionFailure::StorageClassification;
            out.error = "orphan anchor without its journal (seq " +
                        std::to_string(a) + "); the storage state is ambiguous";
            return out;
        }
    }
    if (max_seq == kMaxStorageSequence) {
        out.failure = JournalSessionFailure::SequenceOverflow;
        out.error = "the 20-digit root_run_seq space is exhausted; no new runs "
                    "can be allocated for this root";
        return out;
    }
    const unsigned long long next_seq = saw_any || !journal_seqs.empty() ? max_seq + 1 : 1ULL;

    // 4. Names (20-digit zero-padded: lexicographic == numeric).
    char journal_name[64] = {};
    char anchor_name[64] = {};
    std::snprintf(journal_name, sizeof(journal_name), "journal-%020llu.jsonl",
                  static_cast<unsigned long long>(next_seq));
    std::snprintf(anchor_name, sizeof(anchor_name), "anchor-%020llu.jsonl",
                  static_cast<unsigned long long>(next_seq));

    // 5. Capacity admission (before any file creation; under the lock).
    {
        JournalRunStart reserve_start;
        reserve_start.run = "run-" + std::to_string(next_seq);
        reserve_start.root_run_seq = next_seq;
        reserve_start.planned_move_count = planned_move_count;
        reserve_start.lock_id = lock.lock_id;                       // 64 hex (size-stable)
        reserve_start.root_id = root.root_key;                      // 64 hex
        reserve_start.anchor_id = std::string(64, '0');             // size-stable placeholder
        reserve_start.volume = organized_root.volume.volume_guid_path;
        reserve_start.root_path = organized_root.resolved_path_utf8;
        if (undo_window_acknowledged) {
            reserve_start.undo_window_acknowledged = true;
            reserve_start.supersedes_root_run_seq = supersedes_root_run_seq;
            reserve_start.has_ack = true;
        }
        const auto fwd = forward_reserve_bytes(reserve_start, planned_move_count);
        if (!fwd.ok) {
            out.failure = JournalSessionFailure::CapacityExhausted;
            out.error = "forward capacity reserve could not be computed: " + fwd.error;
            return out;
        }
        const auto anchor = anchor_reserve_bytes(planned_move_count);
        if (!anchor.ok) {
            out.failure = JournalSessionFailure::CapacityExhausted;
            out.error = "anchor capacity reserve could not be computed: " + anchor.error;
            return out;
        }
        const auto quota = check_aggregate_quota(root, fwd.bytes + anchor.bytes);
        if (!quota.ok) {
            out.failure = JournalSessionFailure::CapacityExhausted;
            out.error = quota.error;
            out.native_code = quota.native_code;
            return out;
        }
    }

    // 6. Journal and anchor leaves (FILE_CREATE refuse-on-exists). The
    // root key and acquired lock id are captured before the storage/lock
    // ownership moves into the session.
    const std::string root_key = root.root_key;
    const std::string acquired_lock_id = lock.lock_id;
    auto journal = create_journal_leaf(root, journal_name);
    if (!journal.ok) {
        out.failure = JournalSessionFailure::LeafCreation;
        out.error = "journal leaf creation failed: " + journal.error;
        out.native_code = journal.native_code;
        return out;
    }
    auto anchor = create_journal_leaf(root, anchor_name);
    if (!anchor.ok) {
        // M1: the journal exists, the anchor does not - the sequence is
        // consumed (abandoned-forward-init on restart; safe, disclosed).
        out.failure = JournalSessionFailure::AnchorCreation;
        out.error = "anchor leaf creation failed (the journal sequence is "
                    "consumed; the journal classifies as abandoned-forward-"
                    "initialization): " + anchor.error;
        out.native_code = anchor.native_code;
        return out;
    }
    if (journal.id_hex.empty() || anchor.id_hex.empty()) {
        out.failure = JournalSessionFailure::Identity;
        out.error = "the journal or anchor identity digest could not be captured";
        return out;
    }

    // 7. RUN-START (flushed) + genesis anchor record (flushed).
    JournalSession session(std::move(root), std::move(lock));
    session.journal_ = std::move(journal);
    session.anchor_ = std::move(anchor);
    session.journal_id_ = session.journal_.id_hex;
    session.anchor_id_ = session.anchor_.id_hex;
    session.root_id_ = root_key;
    session.lock_id_ = acquired_lock_id;
    session.run_ = "run-" + std::to_string(next_seq);
    session.journal_name_ = journal_name;
    session.anchor_name_ = anchor_name;
    session.root_run_seq_ = next_seq;
    // RUN-START occupies record position 0; the first move record is seq 1.
    session.next_seq_ = 1;
    session.open_ = true;

    JournalRunStart start;
    start.run = session.run_;
    start.seq = 0;
    start.root_run_seq = next_seq;
    start.planned_move_count = planned_move_count;
    start.lock_id = session.lock_id_;
    start.root_id = session.root_id_;
    start.anchor_id = session.anchor_id_;
    start.volume = organized_root.volume.volume_guid_path;
    start.root_path = organized_root.resolved_path_utf8;
    if (undo_window_acknowledged) {
        start.undo_window_acknowledged = true;
        start.supersedes_root_run_seq = supersedes_root_run_seq;
        start.has_ack = true;
    }
    const auto genesis = journal_genesis_prev(session.root_id_, next_seq, session.run_);
    if (!genesis.ok) {
        out.failure = JournalSessionFailure::Identity;
        out.error = "the journal genesis seed could not be computed: " + genesis.error;
        return out;
    }
    const auto started = session.append_and_anchor(JournalRecord{start}, genesis.hex);
    if (!started.ok) {
        out.failure = started.failure;
        out.error = started.error;
        out.native_code = started.native_code;
        return out;
    }

    out.ok = true;
    out.session = std::move(session);
    return out;
}

JournalSessionResult JournalSession::append_and_anchor(const JournalRecord& record,
                                                       const std::string& prev_hex) {
    if (!open_) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "the journal session is not open");
    }
    auto serialized = serialize_journal_record(record, prev_hex);
    if (!serialized.ok) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "record serialization failed (invariant violation): " +
                                serialized.error);
    }
    const auto appended =
        append_journal_leaf_bytes(journal_.handle, serialized.canonical);
    if (!appended.ok) {
        return session_fail(JournalSessionFailure::JournalWrite,
                            "journal append failed: " + appended.error,
                            appended.native_code);
    }
    const auto flushed = flush_journal_leaf(journal_.handle);
    if (!flushed.ok) {
        return session_fail(JournalSessionFailure::JournalFlush,
                            "journal flush failed: " + flushed.error,
                            flushed.native_code);
    }
    journal_bytes_ += serialized.canonical.size();

    // The covering anchor record's last_record_hash is the covered record's
    // own integrity, computed by the ratified serializer. The next record's
    // prev chains over this record's full canonical bytes including its
    // integrity member, excluding the LF (the serialize_journal_chain rule).
    const std::string integrity = serialized.integrity_hex;
    std::string_view line_view(serialized.canonical);
    line_view.remove_suffix(1); // LF
    const auto next_prev = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(line_view.data()), line_view.size()));
    if (!next_prev.ok) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "SHA-256 provider failed: " + next_prev.error);
    }
    const std::string next_prev_hex = journal_hex(next_prev.digest);

    unsigned long long covered_seq = 0;
    std::visit([&covered_seq](const auto& rec) { covered_seq = rec.seq; }, record);
    AnchorRecord anchor;
    anchor.root_run_seq = root_run_seq_;
    anchor.journal_id = journal_id_;
    anchor.anchor_seq = anchor_seq_++;
    anchor.covered_seq = covered_seq;
    anchor.byte_len = journal_bytes_;
    anchor.last_record_hash = integrity;

    auto anchor_serialized = serialize_anchor_record(anchor);
    if (!anchor_serialized.ok) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "anchor serialization failed (invariant violation): " +
                                anchor_serialized.error);
    }
    const auto anchor_appended =
        append_journal_leaf_bytes(anchor_.handle, anchor_serialized.canonical);
    if (!anchor_appended.ok) {
        return session_fail(JournalSessionFailure::AnchorWrite,
                            "anchor append failed: " + anchor_appended.error,
                            anchor_appended.native_code);
    }
    const auto anchor_flushed = flush_journal_leaf(anchor_.handle);
    if (!anchor_flushed.ok) {
        return session_fail(JournalSessionFailure::AnchorFlush,
                            "anchor flush failed: " + anchor_flushed.error,
                            anchor_flushed.native_code);
    }

    prev_hex_ = next_prev_hex;
    return JournalSessionResult{true, JournalSessionFailure::Ok, {}, 0};
}

JournalSessionResult JournalSession::record_intent(JournalIntent intent) {
    if (run_ended_) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "the run has ended; no further records are accepted");
    }
    intent.run = run_;
    intent.seq = next_seq_++;
    intent.root_run_seq = root_run_seq_;
    return append_and_anchor(JournalRecord{intent}, prev_hex_);
}

JournalSessionResult JournalSession::record_result(JournalResult result) {
    if (run_ended_) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "the run has ended; no further records are accepted");
    }
    result.run = run_;
    result.seq = next_seq_++;
    result.root_run_seq = root_run_seq_;
    return append_and_anchor(JournalRecord{result}, prev_hex_);
}

JournalSessionResult JournalSession::end_run(const JournalRunEnd& run_end_in) {
    if (run_ended_) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "the run has already ended");
    }
    JournalRunEnd run_end = run_end_in;
    run_end.run = run_;
    run_end.seq = next_seq_++;
    run_end.root_run_seq = root_run_seq_;
    auto serialized = serialize_journal_record(JournalRecord{run_end}, prev_hex_);
    if (!serialized.ok) {
        return session_fail(JournalSessionFailure::InvalidState,
                            "RUN-END serialization failed (invariant violation): " +
                                serialized.error);
    }
    const auto appended = append_journal_leaf_bytes(journal_.handle, serialized.canonical);
    if (!appended.ok) {
        return session_fail(JournalSessionFailure::JournalWrite,
                            "RUN-END append failed: " + appended.error,
                            appended.native_code);
    }
    const auto flushed = flush_journal_leaf(journal_.handle);
    if (!flushed.ok) {
        return session_fail(JournalSessionFailure::JournalFlush,
                            "RUN-END flush failed (the journal classifies as "
                            "complete-unterminated with a durability caveat): " +
                                flushed.error,
                            flushed.native_code);
    }
    journal_bytes_ += serialized.canonical.size();
    run_ended_ = true;
    return JournalSessionResult{true, JournalSessionFailure::Ok, {}, 0};
}

} // namespace folderpilot
