// FolderPilot - Phase 8 retry planning (read-only).
//
// Reads the newest forward chain and classifies each durable RESULT
// outcome into the ratified retry taxonomy (docs/PHASE8-DESIGN.md §1.4):
// Locked/AccessDenied are retryable only after a fresh preflight proves the
// file is still at its source slot with a matching fingerprint; everything
// else is never retried. Strictly read-only for the organized tree and the
// journals. The storage classification (§20.1/§20.2 entry conventions, §4.1
// chain gate including the anchor) runs under the EXCLUSIVE mutation lock:
// a held lock is reported busy (a live writer's torn tail is never called
// corruption, §11/§14), and every classification refusal blocks the retry
// (exit 2) exactly like the apply path.

#pragma once

#include <string>
#include <vector>

#include "journal.hpp"
#include "native.hpp"

namespace folderpilot {

struct RetryCandidate {
    unsigned long long move_id = 0;
    std::string source_name;                    // possibly a recursive relative path
    unsigned long long file_ctime_filetime = 0;
    unsigned long long file_size = 0;
    std::string file_id_hex;                    // the recorded identity digest (v1)
    JournalOutcome recorded_outcome = JournalOutcome::Locked;
    bool preflight_ok = false;                  // fresh fingerprint match confirmed
    std::string preflight_detail;               // mutable: the preflight writes it
};

struct RetryPlanResult {
    bool ok = false;                    // false: planning could not complete (exit 3)
    std::string error;
    bool busy = false;                  // the mutation lock is held by a live run (exit 2)
    bool blocked = false;               // the newest chain is blocked/refused (exit 2)
    std::string blocked_detail;
    unsigned long long root_run_seq = 0;
    std::string journal_name;
    std::vector<RetryCandidate> candidates;   // preflighted; preflight_ok == true
    std::size_t not_retryable = 0;      // Moved/permanent/identity outcomes
    std::size_t preflight_failed = 0;   // vanished or fingerprint-mismatched
};

RetryPlanResult load_retry_plan(std::string_view storage_base_path,
                                const platform::RootDirectory& organized_root);

} // namespace folderpilot
