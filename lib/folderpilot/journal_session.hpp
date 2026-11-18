// FolderPilot - Phase 7.2 write-ahead journal session (one forward run).
//
// Owns the per-root storage chain for a single journaled forward run:
// storage root, exclusive mutation lock, the run's journal and anchor leaves,
// and the write-ahead record chain (RUN-START -> per-move INTENT/RESULT ->
// RUN-END) with the ratified anchor ordering (journal append -> journal flush
// -> anchor append -> anchor flush -> next permitted operation; INTENT
// anchors before its rename; RESULT anchors before the next move; RUN-END is
// an unanchored terminal, flush only).
//
// Every durable-state classification follows docs/PHASE7-DESIGN.md §20.6
// (matrix M0-M13) and the ratified §4/§6/§7/§10 semantics. No journal,
// anchor or sequence is ever reused; any storage-classification ambiguity
// refuses new mutation; every write/flush/anchor failure is a structured R1
// stop with the durable state classified - the caller reports it, never
// silently continues, and the session refuses further records after a stop.

#pragma once

#include <string>

#include "folderpilot/journal.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"
#include "unique_handle.hpp"

namespace folderpilot {

// Session-level failure taxonomy (maps onto the §20.8 exit-code rules:
// safety classifications surface to the CLI as exit 2, capacity and
// storage-I/O as exit 3).
enum class JournalSessionFailure {
    Ok,
    StorageInvalid,        // the organized root lacks volume/identity material
    StorageClassification, // unrecognized/malformed storage entries, orphan anchor, unsafe state (exit 2)
    SequenceOverflow,      // the 20-digit sequence space is exhausted (exit 3)
    LockHeld,              // another FolderPilot operation holds the mutation lock (exit 2)
    LockFailure,           // the lock could not be acquired for another reason (exit 2)
    CapacityExhausted,     // the aggregate quota cannot admit the reserve (exit 3)
    LeafCreation,          // the journal FILE_CREATE failed (exit 3)
    AnchorCreation,        // the anchor FILE_CREATE failed after the journal was created (exit 3; seq consumed, M1)
    Identity,              // a leaf identity digest could not be captured (exit 2)
    JournalWrite,          // an INTENT/RESULT/RUN-START append failed (W1; exit 3 stop)
    JournalFlush,          // the journal flush failed (W2; exit 3 stop)
    AnchorWrite,           // the anchor append failed (W4; exit 3 stop)
    AnchorFlush,           // the anchor flush failed (W5; exit 3 stop)
    InvalidState,          // session-level invariant violation (never expected; fail closed)
};

struct JournalSessionResult {
    bool ok = false;
    JournalSessionFailure failure = JournalSessionFailure::InvalidState;
    std::string error;
    unsigned long native_code = 0;
};

// Test-only fail-injection seam for the ratified §20.6 matrix windows
// (T-86..T-95): production always passes the default (no faults). Counters
// are 1-based over the session's durable-record operations.
struct JournalSessionFaults {
    int fail_journal_append_at = -1;   // W1: the Nth journal append fails
    int fail_journal_flush_at = -1;    // W2: the Nth journal flush fails
    int fail_anchor_append_at = -1;    // W4: the Nth anchor append fails
    int fail_anchor_flush_at = -1;     // W5: the Nth anchor flush fails
};

struct JournalSessionOpenResult;

// The write-ahead journal session for one forward run. Move-only: the
// storage handles are uniquely owned for the run's lifetime.
class JournalSession {
public:
    JournalSession() = default;
    JournalSession(JournalSession&&) noexcept = default;
    JournalSession& operator=(JournalSession&&) noexcept = default;
    JournalSession(const JournalSession&) = delete;
    JournalSession& operator=(const JournalSession&) = delete;

    // Classifies the per-root storage, allocates the next root_run_seq,
    // admits the capacity reserve, creates the journal and anchor leaves,
    // captures their identities, and writes RUN-START (flushed) plus the
    // genesis anchor record (flushed). Every failure leaves the durable
    // state classified per §20.6 and refuses the run.
    static JournalSessionOpenResult open_for_run(
        std::string_view storage_base_path,
        const platform::RootDirectory& organized_root,
        unsigned long long planned_move_count,
        bool undo_window_acknowledged,
        unsigned long long supersedes_root_run_seq,
        const JournalSessionFaults& faults = {});

    // Writes INTENT (serialized with the running prev chain), flushes the
    // journal, appends the covering anchor record, flushes the anchor. The
    // session fills run/seq/root_run_seq; the caller supplies the move
    // payload (identity digests, ctime, names, category).
    JournalSessionResult record_intent(JournalIntent intent);

    // Same ordering for a RESULT (outcome of the just-intended move).
    JournalSessionResult record_result(JournalResult result);

    // Writes RUN-END (unanchored terminal) and flushes the journal only.
    // The session refuses further records afterwards.
    JournalSessionResult end_run(const JournalRunEnd& run_end);

    // Identity accessors for the executor (RUN-START cross-binding).
    const std::string& lock_id() const { return lock_id_; }
    const std::string& journal_id() const { return journal_id_; }
    const std::string& anchor_id() const { return anchor_id_; }
    const std::string& root_id() const { return root_id_; }
    const std::string& run() const { return run_; }
    unsigned long long root_run_seq() const { return root_run_seq_; }
    const std::string& journal_name() const { return journal_name_; }
    const std::string& anchor_name() const { return anchor_name_; }

private:
    JournalSession(StorageRoot root, MutationLock lock);

    // Serializes `record` onto `prev_hex` (the genesis seed for RUN-START,
    // the running chain otherwise), appends + flushes the journal, extracts
    // the record's ratified integrity via the parser, then appends + flushes
    // the covering anchor record. Advances the chain state.
    JournalSessionResult append_and_anchor(const JournalRecord& record,
                                           const std::string& prev_hex);

    StorageRoot root_{};
    MutationLock lock_{};
    StorageLeaf journal_{};
    StorageLeaf anchor_{};
    std::string lock_id_;
    std::string journal_id_;
    std::string anchor_id_;
    std::string root_id_;
    std::string run_;
    std::string journal_name_;
    std::string anchor_name_;
    unsigned long long root_run_seq_ = 0;
    unsigned long long next_seq_ = 0;
    unsigned long long intents_written_ = 0;
    unsigned long long anchor_seq_ = 0;
    unsigned long long journal_bytes_ = 0;
    std::string prev_hex_;
    bool open_ = false;
    bool run_ended_ = false;
    bool stopped_ = false;             // R1 latch: no records after a durable failure
    JournalSessionFaults faults_{};
    unsigned long long journal_appends_ = 0;
    unsigned long long journal_flushes_ = 0;
    unsigned long long anchor_appends_ = 0;
    unsigned long long anchor_flushes_ = 0;
};

// Result of open_for_run: either an open session or a classified failure
// (the durable state per §20.6 is disclosed through failure/error).
struct JournalSessionOpenResult {
    bool ok = false;
    JournalSessionFailure failure = JournalSessionFailure::StorageInvalid;
    std::string error;
    unsigned long native_code = 0;
    JournalSession session;   // valid when ok
};

} // namespace folderpilot
