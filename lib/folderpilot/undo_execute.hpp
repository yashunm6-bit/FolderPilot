// FolderPilot - Phase 7.3 undo execution (explicitly authorized reverse runs).
//
// Consumes a ratified undo plan (Phase 7.2 read-only planning), acquires the
// per-root mutation lock, re-plans under the lock, and executes the reverse
// moves as a separate append-only undo-attempt segment (G9: segments are
// UNANCHORED - no anchor records; §6.2 ordering: UNDO-START flushed, per
// candidate UNDO-INTENT flushed then the immediately-revalidated no-replace
// reverse rename then UNDO-RESULT flushed, UNDO-END flushed last).
//
// Reverse moves reuse the generic handle-relative no-replace rename: the
// destination file's open handle is the rename source; the retained root is
// the destination directory; the recorded source name is the destination
// name. A refilled source slot is therefore an OS-enforced Conflict (R16:
// an unsafe repeated reversal is structurally impossible).
//
// Every candidate is revalidated immediately before its reverse rename
// (§9.3: the plan is not an atomic snapshot). Candidates whose first
// revalidation refuses are reported but not journaled (§6.2: the refusal
// happens before the candidate's UNDO-INTENT). A durable-record failure
// stops the segment per R1 - the file state stays as the last confirmed
// journal evidence describes it.

#pragma once

#include <string>
#include <vector>

#include "folderpilot/undo_plan.hpp"

namespace folderpilot {

struct UndoMoveOutcomeRecord {
    unsigned long long move_id = 0;
    std::string source_name;
    std::string dest_name;
    std::string outcome;      // "restored", or the ratified failure spelling
    std::string detail;
    bool journaled = false;   // false: refused before the candidate's INTENT
};

struct UndoExecutionResult {
    bool aborted = false;             // pre-mutation refusal (no segment created)
    int abort_exit_code = 2;          // ratified mapping: 2 safety, 3 operational
    std::string abort_reason;
    bool segment_active = false;      // an undo-attempt segment was created
    bool segment_stopped = false;     // R1: a durable-record failure stopped the segment
    std::string segment_error;
    unsigned long native_code = 0;    // preserved from the failing step
    std::size_t eligible_count = 0;   // candidates that reached the segment
    std::size_t restored_count = 0;
    std::size_t conflict_count = 0;
    std::size_t failed_count = 0;
    std::size_t refused_count = 0;    // reconciliation refusals (not journaled)
    bool ok_shape = false;            // true: the run completed with nothing eligible
    std::vector<UndoMoveOutcomeRecord> records;

    bool all_restored() const {
        return !aborted && !segment_stopped && restored_count == eligible_count &&
               conflict_count == 0 && failed_count == 0;
    }
};

// Executes the undo for the loaded plan. `mutation_authorized` must be true
// (the CLI's --undo --apply); without it the call refuses before touching
// anything - the undo preview path uses load_undo_plan directly and can
// never reach this function.
UndoExecutionResult execute_undo(const UndoPlanResult& plan,
                                 std::string_view storage_base_path,
                                 const platform::RootDirectory& organized_root,
                                 bool mutation_authorized);

} // namespace folderpilot
