// FolderPilot - safe file executor implementation.

#include "folderpilot/executor.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

#include "folderpilot/planner.hpp" // plan invariant validation
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
                             std::string_view requested_path) {
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

    // ---- 3. Destination categories: revalidate or create (root-relative).
    // Retained category handles own their RAII for the whole execution.
    // A category that fails revalidation/creation records its outcome once
    // per planned move in the per-move loop (no double emission).
    struct RetainedCategory {
        Category category;
        platform::unique_handle handle;
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
        retained.push_back({status.category, std::move(category.handle)});
    }

    // ---- 4. Per-move execution (failures are isolated).
    for (const PlannedMove& move : plan.moves()) {
        // Category-level failures first: one structured record per move.
        const auto failed = std::find_if(
            failed_categories.begin(), failed_categories.end(),
            [&move](const FailedCategory& f) {
                return f.category == move.destination_category;
            });
        if (failed != failed_categories.end()) {
            result.records.push_back(
                record(move, failed->outcome, failed->detail, failed->native_code));
            continue;
        }

        const HANDLE destination = category_handle(move.destination_category);
        if (destination == nullptr) {
            // Unsafe/Uninspectable categories were excluded from the plan.
            result.records.push_back(record(move, MoveOutcome::DestinationUnsafe,
                                            "destination category is unsafe", 0));
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
            result.records.push_back(record(move, outcome, source.error, source.native_code));
            continue;
        }

        // Handle-based safety gate (the opened handle is the source of truth).
        if (source.attributes.reparse_point()) {
            result.records.push_back(record(move, MoveOutcome::SourceReplaced,
                                            "source is a reparse point", 0));
            continue;
        }
        if (source.attributes.hidden() || source.attributes.system()) {
            result.records.push_back(record(move, MoveOutcome::SourceUnsafe,
                                            "source is hidden or system", 0));
            continue;
        }

        // Identity revalidation: representation and values must both match.
        if (!platform::identity_matches(source.identity, move.identity)) {
            const bool representation_mismatch =
                source.identity.index() != move.identity.index();
            result.records.push_back(record(
                move,
                representation_mismatch ? MoveOutcome::SourceInvalid
                                        : MoveOutcome::SourceReplaced,
                representation_mismatch
                    ? "source identity representation differs (fail closed)"
                    : "source file was replaced after planning",
                0));
            continue;
        }

        // Volume guard: the source must live on the root's volume. A
        // representation mismatch fails closed.
        if (source.identity.index() != root.identity.index()) {
            result.records.push_back(record(move, MoveOutcome::SourceInvalid,
                                            "source identity representation differs (fail closed)",
                                            0));
            continue;
        }
        if (std::holds_alternative<platform::SourceIdentityEx>(source.identity) &&
            std::get<platform::SourceIdentityEx>(source.identity).volume_serial !=
                std::get<platform::SourceIdentityEx>(root.identity).volume_serial) {
            result.records.push_back(record(move, MoveOutcome::CrossVolume,
                                            "source volume differs from the root volume", 0));
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
            result.records.push_back(record(move, outcome, renamed.error, renamed.native_code));
            continue;
        }
        result.records.push_back(record(move, MoveOutcome::Moved, {}, 0));
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
    return result;
}

} // namespace folderpilot
