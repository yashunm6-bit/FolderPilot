// FolderPilot - dry-run reporter implementation.

#include "folderpilot/reporter.hpp"

#include <sstream>

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
