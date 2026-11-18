// FolderPilot - Phase 7.2 undo planning (read-only).
//
// Selects the newest forward chain in the per-root storage, classifies it
// against its anchor (ratified §4.1/§6.3/§7/§10), folds the forward results
// with every undo-attempt segment (§9.1), and reconciles each live candidate
// against the current filesystem (§9.3). Strictly read-only: no journal
// writes, no renames, no lock acquisition.
//
// Output classes (§20.7 W4): every class other than Undoable/AlreadyReversed
// is a refusal for the corresponding move.

#pragma once

#include <string>
#include <vector>

#include "folderpilot/category.hpp"
#include "folderpilot/journal.hpp"
#include "native.hpp"

namespace folderpilot {

enum class UndoCandidateClass {
    Undoable,         // matching identity at the destination; source slot free (§9.3)
    AlreadyReversed,  // matching identity at the original source slot; absent from the destination
    Conflict,         // the destination slot is occupied by a replacement (§9.3 refusal)
    Missing,          // both destination and source slot are absent (§9.3 refusal)
    Replaced,         // destination identity does not match the recorded fingerprint (refusal)
    Ambiguous,        // both occupied, both absent, or otherwise indistinguishable
    Blocked,          // the chain/segment state blocks this move
    Corrupt,          // the journal or anchor is corrupt
};

struct UndoCandidate {
    unsigned long long move_id = 0;
    std::string source_name;
    std::string dest_name;
    Category dest_cat{};
    unsigned long long file_ctime_filetime = 0;   // the recorded gating fingerprint
    std::string file_id_hex;                      // the recorded identity digest (v1)
    unsigned long long file_size = 0;
    UndoCandidateClass classification = UndoCandidateClass::Ambiguous;
    std::string detail;                           // human-readable reconciliation evidence
};

struct UndoCandidateFold {
    // The newest forward chain's ratified classification.
    ForwardChainState state = ForwardChainState::Blocked;
    ForwardBlockReason reason = ForwardBlockReason::ParseInvalid;
    AckWindowStatus ack_window = AckWindowStatus::None;
    bool durability_caveat = false;
    bool torn_terminal_end = false;
    std::string detail;
    // The newest attempt segment's state, when one exists.
    bool has_segments = false;
    SegmentState newest_segment_state = SegmentState::Blocked;
    // Per-move fold output (§9.1/§9.3).
    std::vector<UndoCandidate> candidates;
};

struct UndoPlanResult {
    bool ok = false;                  // false: the storage could not be read/classified
    UndoCandidateFold fold;           // meaningful when ok (blocked states are ok=true)
    std::string error;                // storage/read failure detail when !ok
    unsigned long long root_run_seq = 0;
    std::string journal_name;
    std::string anchor_name;
};

// Read-only undo planning: no lock acquisition, no journal writes, no
// renames. The newest forward chain is selected by name enumeration; prior
// attempt segments fold per §9.1; live candidates reconcile per §9.3.
UndoPlanResult load_undo_plan(std::string_view storage_base_path,
                              const platform::RootDirectory& organized_root);

} // namespace folderpilot
