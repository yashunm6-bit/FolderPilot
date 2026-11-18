// FolderPilot - safe file executor implementation.

#include "folderpilot/executor.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

#include "folderpilot/journal_session.hpp"
#include "folderpilot/planner.hpp" // plan invariant validation
#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"

namespace folderpilot {

namespace {

MoveRecord record(const PlannedMove& move, MoveOutcome outcome,
                  std::string detail, unsigned long native_code = 0) {
    return MoveRecord{move.source_name, move.destination_category,
                      move.destination_name, outcome, std::move(detail), native_code};
}

bool is_skipped_outcome(MoveOutcome outcome) {
    switch (outcome) {
        case MoveOutcome::SourceMissing:
        case MoveOutcome::SourceReplaced:
        case MoveOutcome::SourceUnsafe:
        case MoveOutcome::DestinationUnsafe:
        case MoveOutcome::DestinationChanged:
            return true;
        default:
            return false;
    }
}

} // namespace

ExecutionResult execute_plan(const OrganizationPlan& plan,
                             std::string_view requested_path,
                             std::string_view storage_base_path) {
    ExecutionResult result;

    // ---- 1. Batch-level validation (nothing has been mutated yet). ----
    if (!plan.valid()) {
        result.aborted = true;
        result.abort_reason = "plan is invalid: " + plan.error();
        return result;
    }
    if (const auto violation =
            validate_plan_invariants(plan.moves(), plan.categories());
        violation.has_value()) {
        result.aborted = true;
        result.abort_reason = "plan invariant violated: " + *violation;
        return result;
    }
    result.planned_count = plan.planned_count();

    // ---- 2. Root revalidation: fresh bootstrap through the approved
    //         PathGuard mechanism. The handle is retained for the whole
    //         execution window; the root is never reopened by path.
    const auto opened = platform::open_root(requested_path);
    if (!opened.ok) {
        result.aborted = true;
        result.abort_reason = "root bootstrap failed: " + opened.error;
        return result;
    }
    const platform::RootDirectory& root = opened.root;

    // Identity, resolved path and volume must match the plan exactly.
    if (!platform::identity_matches(root.identity, plan.root().identity) ||
        platform::compare_ordinal_ci(root.resolved_path_utf8,
                                     plan.root().resolved_path) != 0 ||
        root.volume.volume_guid_path != plan.root().volume_guid_path) {
        result.aborted = true;
        result.abort_reason =
            "RootChanged: root identity, path or volume no longer matches the plan";
        return result;
    }

    // ---- 2b. Journal session (Phase 7.2, §20): every mutation is journaled.
    // The session covers lock acquisition, storage classification, sequence
    // allocation, capacity admission and the RUN-START/genesis-anchor
    // write-ahead (§20.6 M0-M4) - all before any category revalidation or
    // move, per §6.1. Refusals here mutate nothing.
    JournalSession session;
    bool journal_active = false;
    if (storage_base_path.empty()) {
        result.aborted = true;
        result.abort_reason = "storage base unavailable: forward mutation is "
                              "only possible through a journaled session";
        return result;
    }
    {
        auto opened = JournalSession::open_for_run(
            storage_base_path, root, plan.planned_count(), false, 0);
        if (!opened.ok) {
            result.aborted = true;
            result.abort_reason = "journal session refused: " + opened.error;
            result.abort_exit_code =
                (opened.failure == JournalSessionFailure::CapacityExhausted ||
                 opened.failure == JournalSessionFailure::SequenceOverflow ||
                 opened.failure == JournalSessionFailure::LeafCreation ||
                 opened.failure == JournalSessionFailure::AnchorCreation)
                    ? 3
                    : 2;
            return result;
        }
        session = std::move(opened.session);
        journal_active = true;
        result.journal_active = true;
    }

    // ---- 3. Destination categories: revalidate or create (root-relative).
    // Retained category handles own their RAII for the whole execution.
    // A category that fails revalidation/creation records its outcome once
    // per planned move in the per-move loop (no double emission).
    struct RetainedCategory {
        Category category;
        platform::unique_handle handle;
        platform::SourceIdentity identity;   // the journal's cat_id source
    };
    std::vector<RetainedCategory> retained;
    struct FailedCategory {
        Category category;
        MoveOutcome outcome;
        std::string detail;
        unsigned long native_code;
    };
    std::vector<FailedCategory> failed_categories;
    auto category_handle = [&retained](Category category) -> HANDLE {
        for (const RetainedCategory& r : retained) {
            if (r.category == category) {
                return r.handle.get();
            }
        }
        return nullptr;
    };

    for (const CategoryStatus& status : plan.categories()) {
        if (status.state == CategoryDirectoryState::Unsafe ||
            status.state == CategoryDirectoryState::Uninspectable) {
            // The plan already excluded these categories; their moves are
            // reported per move below.
            continue;
        }

        const bool was_missing = status.state == CategoryDirectoryState::Missing;
        auto category = platform::open_category_directory(
            root.get(), category_name(status.category), was_missing);
        if (!category.ok) {
            // Missing-at-planning + collision => the category appeared after
            // planning: never adopt it. Deleted => DestinationChanged.
            const MoveOutcome outcome =
                (category.failure == platform::NativeFailure::Conflict ||
                 category.failure == platform::NativeFailure::ConfirmedNotFound)
                    ? MoveOutcome::DestinationChanged
                    : MoveOutcome::Failed;
            failed_categories.push_back({status.category, outcome,
                                         "category revalidation failed: " + category.error,
                                         category.native_code});
            continue;
        }
        if (!category.attributes.directory() || category.attributes.reparse_point() ||
            category.attributes.hidden() || category.attributes.system()) {
            failed_categories.push_back({status.category, MoveOutcome::DestinationUnsafe,
                                         "category directory failed the safety gate after planning",
                                         0});
            continue;
        }
        if (!was_missing && status.has_directory_identity &&
            !platform::identity_matches(status.directory_identity,
                                        category.identity)) {
            // The directory was replaced after planning - never adopt a
            // replacement.
            failed_categories.push_back({status.category, MoveOutcome::DestinationChanged,
                                         "category directory identity no longer matches the plan",
                                         0});
            continue;
        }
        if (was_missing) {
            result.created_directories.push_back(
                std::string{category_name(status.category)});
        }
        retained.push_back({status.category, std::move(category.handle),
                            std::move(category.identity)});
    }

    // Destination-category identity digests for the journal records: the
    // validated handle's identity (§20.3); for categories that failed
    // validation the ratified unvalidated-destination convention applies
    // (64 zeros - the move's RESULT carries the failure outcome and no
    // rename occurs).
    std::vector<std::pair<Category, std::string>> category_digests;
    for (const RetainedCategory& r : retained) {
        std::string digest;
        if (!storage_leaf_id(r.identity, digest)) {
            digest = std::string(64, '0');
        }
        category_digests.push_back({r.category, std::move(digest)});
    }
    auto category_digest = [&](const Category& category) -> std::string {
        for (const auto& [cat, digest] : category_digests) {
            if (cat == category) {
                return digest;
            }
        }
        return std::string(64, '0');
    };
    auto file_digest = [](const platform::SourceIdentity& identity) -> std::string {
        std::string digest;
        if (!storage_leaf_id(identity, digest)) {
            return std::string(64, '0');
        }
        return digest;
    };

    // Journal helpers with ratified R1 stop semantics: a durable-record
    // failure stops the run, classifies the state and never claims success.
    bool journal_stopped = false;
    auto journal_stop = [&](const JournalSessionResult& r) {
        journal_stopped = true;
        result.journal_stopped = true;
        result.journal_error = r.error;
    };
    auto journal_outcome = [](MoveOutcome outcome) -> JournalOutcome {
        // The forward outcome names mirror the executor's MoveOutcome
        // spelling one-to-one (journal.hpp).
        switch (outcome) {
            case MoveOutcome::Moved: return JournalOutcome::Moved;
            case MoveOutcome::Conflict: return JournalOutcome::Conflict;
            case MoveOutcome::SourceMissing: return JournalOutcome::SourceMissing;
            case MoveOutcome::SourceReplaced: return JournalOutcome::SourceReplaced;
            case MoveOutcome::SourceUnsafe: return JournalOutcome::SourceUnsafe;
            case MoveOutcome::Locked: return JournalOutcome::Locked;
            case MoveOutcome::AccessDenied: return JournalOutcome::AccessDenied;
            case MoveOutcome::DestinationUnsafe: return JournalOutcome::DestinationUnsafe;
            case MoveOutcome::DestinationChanged: return JournalOutcome::DestinationChanged;
            case MoveOutcome::CrossVolume: return JournalOutcome::CrossVolume;
            case MoveOutcome::DestinationInvalid: return JournalOutcome::DestinationInvalid;
            case MoveOutcome::SourceInvalid: return JournalOutcome::SourceInvalid;
            case MoveOutcome::Failed: return JournalOutcome::Failed;
        }
        return JournalOutcome::Failed;
    };

    // ---- 4. Per-move execution (failures are isolated; every planned move
    // receives its INTENT in plan order — whatever its eventual outcome —
    // preserving the §6.3 prefix discipline; a rename happens only when the
    // move fully revalidates).
    auto record_intent_for = [&](std::size_t index) -> bool {
        const PlannedMove& move = plan.moves()[index];
        JournalIntent intent;
        intent.move_id = index;
        intent.cat_id = category_digest(move.destination_category);
        intent.file_id = file_digest(move.identity);
        intent.file_ctime = move.creation_time_filetime;
        intent.file_size = move.size_bytes;
        intent.file_mtime = move.last_write_time;
        intent.source_name = move.source_name;
        intent.dest_name = move.destination_name;
        intent.dest_cat = move.destination_category;
        const auto r = session.record_intent(intent);
        if (!r.ok) {
            journal_stop(r);
            return false;
        }
        return true;
    };
    auto record_result_for = [&](std::size_t index, const PlannedMove& move,
                                 MoveOutcome outcome,
                                 const std::string& detail) -> bool {
        JournalResult jr;
        jr.move_id = index;
        jr.cat_id = category_digest(move.destination_category);
        jr.file_id = file_digest(move.identity);
        jr.file_ctime = move.creation_time_filetime;
        jr.file_size = move.size_bytes;
        jr.file_mtime = move.last_write_time;
        jr.source_name = move.source_name;
        jr.dest_name = move.destination_name;
        jr.dest_cat = move.destination_category;
        jr.outcome = journal_outcome(outcome);
        jr.detail = detail.size() > 512 ? detail.substr(0, 512) : detail;
        const auto r = session.record_result(jr);
        if (!r.ok) {
            journal_stop(r);
            return false;
        }
        return true;
    };

    bool run_stopped = false;
    std::size_t index = 0;
    for (; index < plan.moves().size() && !run_stopped; ++index) {
        const PlannedMove& move = plan.moves()[index];

        // INTENT first, durable and anchored before anything else happens to
        // the move (§6.1).
        if (journal_active && !record_intent_for(index)) {
            run_stopped = true;
            break;
        }

        // Emits the move's structured outcome locally, then its RESULT
        // record; a journal failure stops the run (R1) with the outcome
        // still reported honestly.
        auto emit = [&](MoveOutcome outcome, const std::string& detail,
                        unsigned long native_code) {
            result.records.push_back(record(move, outcome, detail, native_code));
            if (journal_active &&
                !record_result_for(index, move, outcome, detail)) {
                run_stopped = true;
            }
        };

        // Category-level failures first: one structured record per move.
        const auto failed = std::find_if(
            failed_categories.begin(), failed_categories.end(),
            [&move](const FailedCategory& f) {
                return f.category == move.destination_category;
            });
        if (failed != failed_categories.end()) {
            emit(failed->outcome, failed->detail, failed->native_code);
            if (run_stopped) {
                break;
            }
            continue;
        }

        const HANDLE destination = category_handle(move.destination_category);
        if (destination == nullptr) {
            // Unsafe/Uninspectable categories were excluded from the plan.
            emit(MoveOutcome::DestinationUnsafe, "destination category is unsafe", 0);
            if (run_stopped) {
                break;
            }
            continue;
        }

        // Open the source relative to the retained root; the handle stays
        // open until the rename completes and closes via RAII.
        auto source = platform::open_source(root.get(), move.source_name);
        if (!source.ok) {
            MoveOutcome outcome = MoveOutcome::Failed;
            switch (source.failure) {
                case platform::NativeFailure::ConfirmedNotFound:
                case platform::NativeFailure::DeletePending:
                    outcome = MoveOutcome::SourceMissing; break;
                case platform::NativeFailure::SharingConflict:
                    outcome = MoveOutcome::Locked; break;
                case platform::NativeFailure::AccessDenied:
                    outcome = MoveOutcome::AccessDenied; break;
                case platform::NativeFailure::IsADirectory:
                case platform::NativeFailure::NotADirectory:
                    outcome = MoveOutcome::SourceReplaced; break;
                case platform::NativeFailure::InvalidPath:
                    outcome = MoveOutcome::SourceInvalid; break;
                default:
                    outcome = MoveOutcome::Failed; break;
            }
            emit(outcome, source.error, source.native_code);
            if (run_stopped) {
                break;
            }
            continue;
        }

        // Handle-based safety gate (the opened handle is the source of truth).
        if (source.attributes.reparse_point()) {
            emit(MoveOutcome::SourceReplaced, "source is a reparse point", 0);
            if (run_stopped) {
                break;
            }
            continue;
        }
        if (source.attributes.hidden() || source.attributes.system()) {
            emit(MoveOutcome::SourceUnsafe, "source is hidden or system", 0);
            if (run_stopped) {
                break;
            }
            continue;
        }

        // Identity revalidation: representation and values must both match.
        if (!platform::identity_matches(source.identity, move.identity)) {
            const bool representation_mismatch =
                source.identity.index() != move.identity.index();
            emit(representation_mismatch ? MoveOutcome::SourceInvalid
                                         : MoveOutcome::SourceReplaced,
                 representation_mismatch
                     ? "source identity representation differs (fail closed)"
                     : "source file was replaced after planning",
                 0);
            if (run_stopped) {
                break;
            }
            continue;
        }

        // Volume guard: the source must live on the root's volume. A
        // representation mismatch fails closed.
        if (source.identity.index() != root.identity.index()) {
            emit(MoveOutcome::SourceInvalid,
                 "source identity representation differs (fail closed)", 0);
            if (run_stopped) {
                break;
            }
            continue;
        }
        if (std::holds_alternative<platform::SourceIdentityEx>(source.identity) &&
            std::get<platform::SourceIdentityEx>(source.identity).volume_serial !=
                std::get<platform::SourceIdentityEx>(root.identity).volume_serial) {
            emit(MoveOutcome::CrossVolume,
                 "source volume differs from the root volume", 0);
            if (run_stopped) {
                break;
            }
            continue;
        }

        // Handle-based, no-overwrite rename through the retained destination
        // handle. The source handle is held until the rename completes.
        const auto renamed = platform::rename_to_category(
            source.handle.get(), destination, move.destination_name);
        if (!renamed.ok) {
            MoveOutcome outcome = MoveOutcome::Failed;
            switch (renamed.failure) {
                case platform::NativeFailure::Conflict:        outcome = MoveOutcome::Conflict; break;
                case platform::NativeFailure::SharingConflict: outcome = MoveOutcome::Locked; break;
                case platform::NativeFailure::AccessDenied:    outcome = MoveOutcome::AccessDenied; break;
                case platform::NativeFailure::CrossVolume:     outcome = MoveOutcome::CrossVolume; break;
                case platform::NativeFailure::InvalidPath:     outcome = MoveOutcome::DestinationInvalid; break;
                default:                                       outcome = MoveOutcome::Failed; break;
            }
            emit(outcome, renamed.error, renamed.native_code);
            if (run_stopped) {
                break;
            }
            continue;
        }
        emit(MoveOutcome::Moved, {}, 0);
        if (run_stopped) {
            break;
        }
    }

    // R1 stop: the remaining planned moves are not attempted and are
    // reported as failed with the disclosure - never as success.
    if (run_stopped) {
        for (; index < plan.moves().size(); ++index) {
            result.records.push_back(
                record(plan.moves()[index], MoveOutcome::Failed,
                       "run stopped: journal failure - remaining moves not "
                       "attempted (§20.6)",
                       0));
        }
        result.journal_stopped = true;
    }

    // ---- 5. Tally.
    for (const MoveRecord& r : result.records) {
        switch (r.outcome) {
            case MoveOutcome::Moved: ++result.moved_count; break;
            case MoveOutcome::Conflict: ++result.conflict_count; break;
            default:
                if (is_skipped_outcome(r.outcome)) {
                    ++result.skipped_count;
                } else {
                    ++result.failed_count;
                }
                break;
        }
    }

    // ---- 6. RUN-END (unanchored terminal, §6.1/§7): written only while the
    // journal is healthy - a stopped run leaves the END absent, which
    // restart classification handles per §7/§10 (M11).
    if (journal_active && !journal_stopped) {
        JournalRunEnd run_end;
        run_end.planned_count = plan.planned_count();
        run_end.moved_count = result.moved_count;
        run_end.conflict_count = result.conflict_count;
        run_end.skipped_count = result.skipped_count;
        run_end.failed_count = result.failed_count;
        if (const auto r = session.end_run(run_end); !r.ok) {
            journal_stop(r);
        }
    }
    return result;
}

} // namespace folderpilot
