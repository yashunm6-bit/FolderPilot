// FolderPilot - dry-run and execution reporter implementation.

#include "folderpilot/reporter.hpp"

#include <sstream>

#include "folderpilot/executor.hpp"

namespace folderpilot {

namespace {

const char* category_folder_note(CategoryDirectoryState state) {
    switch (state) {
        case CategoryDirectoryState::Safe:        return "existing";
        case CategoryDirectoryState::Missing:     return "will be created";
        case CategoryDirectoryState::Unsafe:      return "UNSAFE";
        case CategoryDirectoryState::Uninspectable: return "UNINSPECTABLE";
    }
    return "?";
}

} // namespace

std::string render_dry_run_report(const OrganizationPlan& plan) {
    std::ostringstream out;

    out << "FOLDERPILOT - DRY RUN\n";
    if (!plan.valid()) {
        out << "PLAN INVALID: " << plan.error() << "\n";
        out << "NO FILES WERE MOVED.\n";
        out << "NO DIRECTORIES WERE CREATED.\n";
        out << "NO FILES WERE DELETED.\n";
        return out.str();
    }

    out << "\nTarget:\n" << plan.root().requested_path << "\n";
    out << "\nResolved:\n" << plan.root().resolved_path << "\n";

    // One section per category, in stable category order.
    for (const Category category : all_categories) {
        const CategoryStatus& status = plan.categories()[static_cast<std::size_t>(category)];

        // Unsafe / uninspectable categories are disclosed even with no moves.
        if ((status.state == CategoryDirectoryState::Unsafe ||
             status.state == CategoryDirectoryState::Uninspectable)) {
            out << "\n" << category_name(category) << " ("
                << category_folder_note(status.state) << "):\n";
            out << "  " << status.detail << "\n";
        }

        bool header_printed = false;
        for (const PlannedMove& move : plan.moves()) {
            if (move.destination_category != category) {
                continue;
            }
            if (!header_printed) {
                out << "\n" << category_name(category) << " ("
                    << category_folder_note(status.state) << "):\n";
                header_printed = true;
            }
            out << "  " << move.source_name << " -> "
                << category_name(category) << "/" << move.destination_name;
            if (move.source_name != move.destination_name) {
                out << "  (collision-resolved)";
            }
            out << "\n";
        }
    }

    // The plan's skip record is complete (including internal "." / ".."
    // noise); the user-facing view shows only meaningful skips.
    std::size_t meaningful_skips = 0;
    for (const SkippedEntry& entry : plan.skipped()) {
        if (entry.reason == SkipReason::ReservedName) {
            continue;
        }
        if (meaningful_skips == 0) {
            out << "\nSkipped:\n";
        }
        ++meaningful_skips;
        out << "  " << (entry.name_utf8.empty() ? std::string{"(unknown name)"} : entry.name_utf8)
            << " - " << entry.detail << "\n";
    }

    out << "\nSummary:\n";
    out << "  Files scanned:             " << plan.scanned_count() << "\n";
    out << "  Planned moves:             " << plan.planned_count() << "\n";
    out << "  Skipped:                   " << meaningful_skips << "\n";
    out << "  Unsafe categories:         " << plan.unsafe_category_count() << "\n";
    out << "  Existing category folders: " << plan.existing_category_count() << "\n";
    out << "  Missing category folders:  " << plan.missing_category_count()
        << " (would be created during a later execution phase)\n";
    out << "  Collision-resolved names:  " << plan.collision_resolved_count() << "\n";

    out << "\nNO FILES WERE MOVED.\n";
    out << "NO DIRECTORIES WERE CREATED.\n";
    out << "NO FILES WERE DELETED.\n";
    return out.str();
}

} // namespace folderpilot

namespace folderpilot {

const char* outcome_name(MoveOutcome outcome) {
    switch (outcome) {
        case MoveOutcome::Moved:              return "Moved";
        case MoveOutcome::Conflict:           return "Conflict";
        case MoveOutcome::SourceMissing:      return "SourceMissing";
        case MoveOutcome::SourceReplaced:     return "SourceReplaced";
        case MoveOutcome::SourceUnsafe:       return "SourceUnsafe";
        case MoveOutcome::Locked:             return "Locked";
        case MoveOutcome::AccessDenied:       return "AccessDenied";
        case MoveOutcome::DestinationUnsafe:  return "DestinationUnsafe";
        case MoveOutcome::DestinationChanged: return "DestinationChanged";
        case MoveOutcome::CrossVolume:        return "CrossVolume";
        case MoveOutcome::DestinationInvalid: return "DestinationInvalid";
        case MoveOutcome::SourceInvalid:      return "SourceInvalid";
        case MoveOutcome::Failed:             return "Failed";
    }
    return "Unknown";
}

std::string render_execution_report(const OrganizationPlan& plan,
                                    const ExecutionResult& result) {
    std::ostringstream out;

    out << "FOLDERPILOT - EXECUTION REPORT\n";
    if (result.aborted) {
        out << "\nEXECUTION ABORTED BEFORE ANY CHANGE:\n  " << result.abort_reason << "\n";
        out << "\nNO FILES WERE MOVED.\n";
        out << "NO DIRECTORIES WERE CREATED.\n";
        out << "NO FILES WERE DELETED.\n";
        return out.str();
    }

    out << "\nTarget:\n" << plan.root().requested_path << "\n";
    out << "\nResolved:\n" << plan.root().resolved_path << "\n";

    for (const Category category : all_categories) {
        bool header_printed = false;
        for (const MoveRecord& r : result.records) {
            if (r.destination_category != category) {
                continue;
            }
            if (!header_printed) {
                out << "\n" << category_name(category) << ":\n";
                header_printed = true;
            }
            out << "  " << r.source_name << " -> " << r.destination_name
                << ": " << outcome_name(r.outcome);
            if (!r.detail.empty()) {
                out << " (" << r.detail << ")";
            }
            out << "\n";
        }
    }

    if (!result.created_directories.empty()) {
        out << "\nDirectories created:\n";
        for (const std::string& name : result.created_directories) {
            out << "  " << name << "\n";
        }
    }

    out << "\nSummary:\n";
    out << "  Planned:             " << result.planned_count << "\n";
    out << "  Moved:               " << result.moved_count << "\n";
    out << "  Conflicts:           " << result.conflict_count << "\n";
    out << "  Skipped:             " << result.skipped_count << "\n";
    out << "  Failed:              " << result.failed_count << "\n";
    out << "  Directories created: " << result.created_directories.size() << "\n";

    if (result.all_moved()) {
        out << "\nExecution completed successfully.\n";
    } else {
        out << "\nExecution completed with partial failures.\n";
    }
    out << "Batch execution is not transactional: each file movement is atomic,\n"
           "partial progress is possible, and undo is not available.\n";
    return out.str();
}

} // namespace folderpilot
