// FolderPilot - Phase 8 retry planning implementation (read-only).

#include "folderpilot/retry_plan.hpp"

#include <cstdio>
#include <optional>
#include <vector>

#include "folderpilot/journal_session.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"

namespace folderpilot {

RetryPlanResult load_retry_plan(std::string_view storage_base_path,
                                const platform::RootDirectory& organized_root) {
    RetryPlanResult out;

    if (!organized_root.valid()) {
        out.error = "retry planning requires a validated organized root";
        return out;
    }
    auto storage = open_storage_root(storage_base_path, organized_root);
    if (!storage.ok) {
        out.error = "storage root: " + storage.error;
        return out;
    }

    // The mutation lock is held for the classification read (§11/§14): a
    // live writer's torn tail is "in progress", never corruption. Failing
    // to acquire reports busy - the retry is unavailable right now (exit 2).
    auto lock = acquire_mutation_lock(storage);
    if (!lock.ok) {
        out.ok = true;
        out.busy = true;
        out.blocked_detail =
            "a FolderPilot run is in progress; the retry is unavailable "
            "until it finishes";
        return out;
    }

    // The FULL storage classification (§20.1/§20.2 entry conventions,
    // §4.1 chain gate including the anchor) - identical rules to the apply
    // path. Any refusal blocks the retry entirely (exit 2, flag-immune).
    const auto chain = classify_storage_and_newest_chain(storage);
    if (!chain.ok) {
        if (chain.failure == JournalSessionFailure::StorageRead) {
            // A storage read failure is operational (exit 3), not a chain
            // refusal - the caller surfaces it through the error path.
            out.error = chain.error;
            return out;
        }
        out.ok = true;
        out.blocked = true;
        out.blocked_detail = chain.error;
        return out;
    }
    if (!chain.newest_journal) {
        out.ok = true;
        out.error = "no forward run exists";
        return out;
    }

    // Fresh preflight per candidate: the file must still exist at its
    // (possibly recursive relative) source slot with a matching fingerprint
    // (identity digest v1 AND creation time).
    for (const JournalRecord& rec : chain.newest_journal->records) {
        const auto* result = std::get_if<JournalResult>(&rec);
        if (result == nullptr) {
            continue;
        }
        if (result->outcome == JournalOutcome::Moved) {
            ++out.not_retryable; // completed moves are never retried
            continue;
        }
        if (result->outcome != JournalOutcome::Locked &&
            result->outcome != JournalOutcome::AccessDenied) {
            // Permanent destination/source states, identity failures and the
            // generic catch-all are never retried (§1.4 taxonomy).
            ++out.not_retryable;
            continue;
        }
        RetryCandidate c;
        c.move_id = result->move_id;
        c.source_name = result->source_name;
        c.file_ctime_filetime = result->file_ctime;
        c.file_id_hex = result->file_id;
        c.file_size = result->file_size;
        c.recorded_outcome = result->outcome;

        // Fresh preflight: the source reopened through the ratified policy.
        auto src = c.source_name.find('\\') != std::string::npos
                       ? platform::open_source_at(organized_root.get(), c.source_name)
                       : platform::open_source(organized_root.get(), c.source_name);
        if (!src.ok) {
            c.preflight_detail =
                "the source could not be reopened: " + src.error;
            ++out.preflight_failed;
        } else {
            std::string live_digest;
            if (storage_leaf_id(src.identity, live_digest) &&
                live_digest == c.file_id_hex &&
                src.creation_time_filetime == c.file_ctime_filetime) {
                c.preflight_ok = true;
            } else {
                c.preflight_detail =
                    "the source fingerprint no longer matches the journal";
                ++out.preflight_failed;
            }
        }
        out.candidates.push_back(std::move(c));
    }
    out.root_run_seq = chain.newest_seq;
    char journal_name[64] = {};
    std::snprintf(journal_name, sizeof(journal_name), "journal-%020llu.jsonl",
                  static_cast<unsigned long long>(chain.newest_seq));
    out.journal_name = journal_name;
    out.ok = true;
    return out;
}

} // namespace folderpilot
