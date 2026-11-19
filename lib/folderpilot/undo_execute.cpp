// FolderPilot - Phase 7.3 undo execution implementation.
//
// The undo-attempt segment writer and the reverse-move executor. Append-only
// segment semantics (G9): the segment is created with FILE_CREATE, written
// forward-only (UNDO-START, UNDO-INTENT/UNDO-RESULT per candidate, UNDO-END)
// and flushed at every step; nothing is ever rewritten, and a durable failure
// stops the segment with the state classified (§6.2/§10 - an incomplete
// segment blocks its chain for forward, per R12).

#include "folderpilot/undo_execute.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "folderpilot/journal.hpp"
#include "folderpilot/journal_session.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"
#include "sha256.hpp"

namespace folderpilot {

namespace {

// The undo-attempt segment writer: lock + segment leaf + the running prev
// chain and counters (the G9 unanchored shape - no anchor records). The
// internal steps report through JournalSessionResult (ok/failure/error/
// native_code); the executor maps failures onto the UndoExecutionResult
// fields at the call sites.
class UndoSegmentSession {
public:
    MutationLock lock;
    StorageLeaf segment;

    // Opens the segment: lock, attempt allocation (max attempt for this
    // forward run + 1), FILE_CREATE, and the running chain state. The
    // forward journal's byte-image digest is supplied by the caller.
    static JournalSessionResult open(UndoSegmentSession& out, const StorageRoot& root,
                                     unsigned long long fwd_seq,
                                     const std::string& fwd_run,
                                     const std::string& fwd_digest,
                                     std::size_t candidate_count) {
        auto lock = acquire_mutation_lock(root);
        if (!lock.ok) {
            return {false, JournalSessionFailure::LockHeld,
                    lock.failure == StorageFailure::SharingViolation
                        ? "another FolderPilot operation holds the mutation lock"
                        : "mutation lock acquisition failed: " + lock.error,
                    lock.native_code};
        }
        // Attempt sequence: max(attempt seq for this forward run) + 1. The
        // ratified naming convention (§20.1); a malformed recognized-looking
        // name would have refused the forward path already.
        unsigned long long max_attempt = 0;
        const auto enumerated = platform::enumerate_immediate_children(
            root.root_key_dir.get(), 64 * 1024,
            [&](const platform::RawDirEntry& entry) {
                if (entry.name == L"." || entry.name == L"..") {
                    return;
                }
                if (entry.name.size() == 52 && entry.name.starts_with(L"undo-") &&
                    entry.name.ends_with(L".jsonl")) {
                    unsigned long long r = 0;
                    unsigned long long a = 0;
                    bool digits_ok = true;
                    for (int i = 0; i < 20; ++i) {
                        const wchar_t c = entry.name[static_cast<std::size_t>(5) + i];
                        if (c < L'0' || c > L'9') {
                            digits_ok = false;
                        }
                        r = r * 10 + static_cast<unsigned long long>(c - L'0');
                    }
                    for (int i = 0; i < 20; ++i) {
                        const wchar_t c = entry.name[static_cast<std::size_t>(26) + i];
                        if (c < L'0' || c > L'9') {
                            digits_ok = false;
                        }
                        a = a * 10 + static_cast<unsigned long long>(c - L'0');
                    }
                    if (digits_ok && r == fwd_seq && a > max_attempt) {
                        max_attempt = a;
                    }
                }
            });
        if (!enumerated.ok) {
            return {false, JournalSessionFailure::StorageClassification,
                    "storage enumeration failed: " + enumerated.error,
                    enumerated.native_code};
        }
        const unsigned long long attempt = max_attempt + 1;
        char seg_name[64] = {};
        std::snprintf(seg_name, sizeof(seg_name), "undo-%020llu-%020llu.jsonl",
                      static_cast<unsigned long long>(fwd_seq),
                      static_cast<unsigned long long>(attempt));

        auto segment = create_journal_leaf(root, seg_name);
        if (!segment.ok) {
            return {false, JournalSessionFailure::LeafCreation,
                    "undo segment creation failed: " + segment.error,
                    segment.native_code};
        }

        out.lock = std::move(lock);
        out.segment = std::move(segment);
        out.attempt_ = attempt;
        out.segment_name_ = seg_name;
        out.fwd_seq_ = fwd_seq;
        out.fwd_run_ = fwd_run;
        out.fwd_digest_ = fwd_digest;
        out.candidate_count_ = candidate_count;
        const auto genesis = journal_genesis_prev(root.root_key, fwd_seq, fwd_run);
        if (!genesis.ok) {
            return {false, JournalSessionFailure::Identity,
                    "the segment genesis seed could not be computed: " + genesis.error,
                    0};
        }
        out.prev_ = genesis.hex;
        out.open_ = true;
        return {true, JournalSessionFailure::Ok, {}, 0};
    }

    // Writes UNDO-START (flushed).
    JournalSessionResult write_start() {
        JournalUndoStart start;
        start.run = fwd_run_;
        start.seq = seq_++;
        start.root_run_seq = fwd_seq_;
        start.fwd_journal_digest = fwd_digest_;
        start.undo_attempt_seq = attempt_;
        start.journal_id = fwd_journal_id_;
        start.attempt_candidate_count = candidate_count_;
        return append(start);
    }

    // Writes UNDO-INTENT (flushed). The destination category's identity
    // digest is supplied by the executor (captured through the validated
    // category handle).
    JournalSessionResult write_intent(const UndoCandidate& c,
                                      const std::string& cat_digest) {
        JournalUndoIntent intent;
        intent.run = fwd_run_;
        intent.seq = seq_++;
        intent.root_run_seq = fwd_seq_;
        intent.fwd_journal_digest = fwd_digest_;
        intent.undo_attempt_seq = attempt_;
        intent.attempt_move_seq = intent_seq_++;
        intent.undo_of_move = c.move_id;
        intent.cat_id = cat_digest;
        intent.file_id = c.file_id_hex;
        intent.file_ctime = c.file_ctime_filetime;
        intent.file_size = c.file_size;
        intent.source_name = c.source_name;
        intent.dest_name = c.dest_name;
        intent.dest_cat = c.dest_cat;
        return append(intent);
    }

    // Writes UNDO-RESULT (flushed).
    JournalSessionResult write_result(const UndoCandidate& c,
                                      const std::string& cat_digest,
                                      JournalOutcome outcome,
                                      const std::string& detail) {
        JournalUndoResult result;
        result.run = fwd_run_;
        result.seq = seq_++;
        result.root_run_seq = fwd_seq_;
        result.fwd_journal_digest = fwd_digest_;
        result.undo_attempt_seq = attempt_;
        result.attempt_move_seq = result_seq_++;
        result.undo_of_move = c.move_id;
        result.cat_id = cat_digest;
        result.file_id = c.file_id_hex;
        result.file_ctime = c.file_ctime_filetime;
        result.file_size = c.file_size;
        result.source_name = c.source_name;
        result.dest_name = c.dest_name;
        result.dest_cat = c.dest_cat;
        result.outcome = outcome;
        result.detail = detail.size() > 512 ? detail.substr(0, 512) : detail;
        return append(result);
    }

    // Writes UNDO-END (flushed; the ratified segment terminal).
    JournalSessionResult write_end(std::size_t restored,
                                   std::size_t refused_or_failed) {
        JournalUndoEnd end;
        end.run = fwd_run_;
        end.seq = seq_++;
        end.root_run_seq = fwd_seq_;
        end.fwd_journal_digest = fwd_digest_;
        end.undo_attempt_seq = attempt_;
        end.planned_count = candidate_count_;
        end.moved_count = restored;
        end.failed_count = refused_or_failed;
        return append(end);
    }

    // R22 cross-binding: the segment records name the forward journal; the
    // digest is supplied by the executor (captured through the journal's
    // ratified read open).
    void set_fwd_journal_id(const std::string& id) { fwd_journal_id_ = id; }

    bool open() const { return open_; }
    const std::string& segment_name() const { return segment_name_; }

private:
    JournalSessionResult append(const JournalRecord& record) {
        if (!open_) {
            return {false, JournalSessionFailure::InvalidState,
                    "the segment is not open", 0};
        }
        if (stopped_) {
            // R1 latch: after a durable failure no further records are
            // accepted (no rewrites, no gaps).
            return {false, JournalSessionFailure::InvalidState,
                    "the segment is stopped after a durable failure; no further "
                    "records are accepted",
                    0};
        }
        auto serialized = serialize_journal_record(record, prev_);
        if (!serialized.ok) {
            stopped_ = true;
            return {false, JournalSessionFailure::InvalidState,
                    "segment record serialization failed (invariant violation): " +
                        serialized.error,
                    0};
        }
        const auto appended = append_journal_leaf_bytes(segment.handle, serialized.canonical);
        if (!appended.ok) {
            stopped_ = true;
            return {false, JournalSessionFailure::JournalWrite,
                    "segment append failed: " + appended.error, appended.native_code};
        }
        const auto flushed = flush_journal_leaf(segment.handle);
        if (!flushed.ok) {
            stopped_ = true;
            return {false, JournalSessionFailure::JournalFlush,
                    "segment flush failed: " + flushed.error, flushed.native_code};
        }
        // prev of the next record = SHA-256 over this record's full canonical
        // bytes including its integrity member, excluding the LF.
        std::string_view line_view(serialized.canonical);
        line_view.remove_suffix(1);
        const auto h = platform::sha256(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(line_view.data()),
            line_view.size()));
        if (!h.ok) {
            stopped_ = true;
            return {false, JournalSessionFailure::JournalWrite,
                    "SHA-256 provider failed: " + h.error, 0};
        }
        prev_ = journal_hex(h.digest);
        return {true, JournalSessionFailure::Ok, {}, 0};
    }

    unsigned long long attempt_ = 0;
    unsigned long long fwd_seq_ = 0;
    std::string fwd_run_;
    std::string fwd_digest_;
    std::string fwd_journal_id_;
    std::size_t candidate_count_ = 0;
    std::string segment_name_;
    std::string prev_;
    unsigned long long seq_ = 0;
    unsigned long long intent_seq_ = 0;
    unsigned long long result_seq_ = 0;
    bool open_ = false;
    bool stopped_ = false;
};

} // namespace

UndoExecutionResult execute_undo(const UndoPlanResult& plan,
                                 std::string_view storage_base_path,
                                 const platform::RootDirectory& organized_root,
                                 bool mutation_authorized) {
    UndoExecutionResult result;

    if (!mutation_authorized) {
        result.aborted = true;
        result.abort_reason =
            "undo execution requires the explicit --undo --apply authorization";
        return result;
    }
    if (!plan.ok) {
        result.aborted = true;
        result.abort_reason = "undo plan: " + plan.error;
        return result;
    }

    // Ratified §9.5: a blocked/corrupt newest chain refuses undo execution
    // (read-only evidence remains available through the preview).
    if (plan.fold.state == ForwardChainState::Blocked) {
        result.aborted = true;
        result.abort_exit_code = 2;
        result.abort_reason = "the newest forward chain is blocked: " +
                              plan.fold.detail;
        return result;
    }
    if (plan.fold.state == ForwardChainState::AbandonedForwardInit ||
        plan.fold.state == ForwardChainState::DeclaredInert) {
        result.aborted = true;
        result.abort_exit_code = 2;
        result.abort_reason = "the newest forward run recorded no moves "
                              "(nothing to undo)";
        return result;
    }

    auto storage = open_storage_root(storage_base_path, organized_root);
    if (!storage.ok) {
        result.aborted = true;
        result.abort_reason = "storage root: " + storage.error;
        return result;
    }

    // The forward journal's identity digest (R22 cross-binding: the segment
    // records name the journal) and its byte-image digest (§5:
    // fwd_journal_digest), both through the ratified read open.
    auto journal_leaf = open_journal_leaf_read(storage, plan.journal_name);
    if (!journal_leaf.ok) {
        result.aborted = true;
        result.abort_reason = "the newest journal could not be reopened";
        return result;
    }
    const std::string fwd_journal_id = journal_leaf.id_hex;
    std::string journal_bytes;
    {
        char buffer[8192];
        DWORD read = 0;
        while (::ReadFile(journal_leaf.handle.get(), buffer, sizeof(buffer), &read,
                          nullptr) &&
               read > 0) {
            journal_bytes.append(buffer, static_cast<std::size_t>(read));
        }
    }
    const auto digest = platform::sha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(journal_bytes.data()),
        journal_bytes.size()));
    if (!digest.ok) {
        result.aborted = true;
        result.abort_reason = "SHA-256 provider failed: " + digest.error;
        return result;
    }
    const std::string fwd_digest = journal_hex(digest.digest);

    // Undoable candidates reach the segment; every other class is a refusal
    // (§20.7 W4) reported through the result without journal records.
    std::vector<UndoCandidate> eligible;
    for (const UndoCandidate& c : plan.fold.candidates) {
        if (c.classification == UndoCandidateClass::Undoable) {
            eligible.push_back(c);
        } else {
            ++result.refused_count;
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name, std::string("refused"),
                c.detail, false});
        }
    }

    // Ratified §9.1: zero live candidates create no segment ("nothing
    // eligible") - the run reports success with nothing restored (T-58).
    if (eligible.empty()) {
        result.abort_reason = "nothing eligible to undo";
        result.ok_shape = true;
        return result;
    }

    // The segment session: lock + attempt allocation + FILE_CREATE.
    UndoSegmentSession session;
    const auto fwd_run = "run-" + std::to_string(plan.root_run_seq);
    auto opened = UndoSegmentSession::open(session, storage, plan.root_run_seq,
                                           fwd_run, fwd_digest, eligible.size());
    if (!opened.ok) {
        result.aborted = true;
        result.abort_exit_code =
            opened.failure == JournalSessionFailure::LockHeld ||
                    opened.failure == JournalSessionFailure::StorageClassification
                ? 2
                : 3;
        result.abort_reason = "undo segment: " + opened.error;
        return result;
    }
    result.segment_active = true;

    // R22 cross-binding: the segment records name the forward journal.
    session.set_fwd_journal_id(fwd_journal_id);

    const auto started = session.write_start();
    if (!started.ok) {
        result.segment_stopped = true;
        result.segment_error = started.error;
        result.native_code = started.native_code;
        return result;
    }

    // Per candidate: revalidate -> UNDO-INTENT -> immediate revalidation ->
    // reverse rename -> UNDO-RESULT (§6.2; §9.3 non-atomic revalidation).
    for (const UndoCandidate& c : eligible) {
        // The destination file, opened through the category directory with
        // the ratified delete-capable source policy (the reverse rename's
        // source handle).
        const auto cat = platform::open_category_directory(
            organized_root.get(), category_name(c.dest_cat), false);
        if (!cat.ok) {
            ++result.refused_count;
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name, std::string("refused"),
                "destination category could not be reopened: " + cat.error, false});
            continue;
        }
        // The destination category's identity digest for the candidate's
        // journal records (captured through the validated handle).
        std::string cat_digest;
        if (!storage_leaf_id(cat.identity, cat_digest)) {
            cat_digest = std::string(64, '0');
        }
        auto dest = platform::open_source(cat.handle.get(), c.dest_name);
        if (!dest.ok) {
            ++result.refused_count;
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name, std::string("refused"),
                "the destination file could not be reopened: " + dest.error, false});
            continue;
        }

        const auto intent = session.write_intent(c, cat_digest);
        if (!intent.ok) {
            result.segment_stopped = true;
            result.segment_error = intent.error;
            result.native_code = intent.native_code;
            break;
        }
        ++result.eligible_count;

        // Immediate revalidation (§9.3): the fingerprint must STILL match -
        // identity digest (v1) AND creation time; any mismatch refuses the
        // reverse rename and is journaled as a failed UNDO-RESULT.
        std::string live_digest;
        const bool fingerprint_ok =
            storage_leaf_id(dest.identity, live_digest) &&
            live_digest == c.file_id_hex &&
            dest.creation_time_filetime == c.file_ctime_filetime;
        if (!fingerprint_ok) {
            const auto written = session.write_result(
                c, cat_digest, JournalOutcome::SourceReplaced,
                "the destination fingerprint no longer matches (fail closed)");
            if (!written.ok) {
                result.segment_stopped = true;
                result.segment_error = written.error;
                result.native_code = written.native_code;
                break;
            }
            ++result.failed_count;
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name, std::string("SourceReplaced"),
                "the destination fingerprint no longer matches (fail closed)", true});
            continue;
        }

        // Reverse rename: the destination file's handle into the retained
        // root under the recorded source name, ReplaceIfExists = FALSE - a
        // refilled source slot is an OS-enforced Conflict (R16).
        const auto renamed = platform::native_rename_to_category(
            dest.handle.get(), organized_root.get(), c.source_name);
        if (!renamed.ok) {
            JournalOutcome outcome = JournalOutcome::Failed;
            if (renamed.failure == platform::NativeFailure::Conflict) {
                outcome = JournalOutcome::Conflict;
                ++result.conflict_count;
            } else {
                ++result.failed_count;
            }
            const auto written =
                session.write_result(c, cat_digest, outcome, renamed.error);
            if (!written.ok) {
                result.segment_stopped = true;
                result.segment_error = written.error;
                result.native_code = written.native_code;
                break;
            }
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name,
                std::string(journal_outcome_name(outcome)), renamed.error, true});
            continue;
        }

        const auto written = session.write_result(c, cat_digest, JournalOutcome::Restored,
                                                  {});
        if (!written.ok) {
            result.segment_stopped = true;
            result.segment_error = written.error;
            result.native_code = written.native_code;
            // The file IS restored; the segment discloses the failed RESULT.
            result.records.push_back(UndoMoveOutcomeRecord{
                c.move_id, c.source_name, c.dest_name, std::string("restored"),
                "restored; the segment RESULT write failed (durability caveat)",
                false});
            ++result.restored_count;
            break;
        }
        ++result.restored_count;
        result.records.push_back(UndoMoveOutcomeRecord{
            c.move_id, c.source_name, c.dest_name, std::string("restored"), {}, true});
    }

    // UNDO-END (the ratified segment terminal) - only while the segment is
    // healthy; a stopped segment leaves the END absent (§6.4 classification).
    if (!result.segment_stopped) {
        const auto ended =
            session.write_end(result.restored_count, result.refused_count);
        if (!ended.ok) {
            result.segment_stopped = true;
            result.segment_error = ended.error;
            result.native_code = ended.native_code;
        }
    }
    return result;
}

} // namespace folderpilot
