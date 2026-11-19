// FolderPilot - dry-run and execution reporter implementation.

#include "folderpilot/reporter.hpp"

#include <sstream>

#include "folderpilot/executor.hpp"
#include "folderpilot/undo_execute.hpp"
#include "folderpilot/undo_plan.hpp"

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


// ---------------------------------------------------------------------------
// Phase 7.3: undo reporting (render-only; the undo paths never mutate here).
// ---------------------------------------------------------------------------

std::string render_undo_preview(const UndoPlanResult& plan) {
    std::ostringstream out;
    out << "UNDO PREVIEW\n";
    if (!plan.ok) {
        out << "  storage could not be read: " << plan.error << "\n";
        return out.str();
    }
    switch (plan.fold.state) {
        case ForwardChainState::Closed:
            out << "  newest run: complete\n"; break;
        case ForwardChainState::CompleteUnterminated:
            out << "  newest run: complete (journal end not flushed)\n"; break;
        case ForwardChainState::PartialUnambiguous:
            out << "  newest run: interrupted after completed moves\n"; break;
        case ForwardChainState::DeclaredInert:
            out << "  newest run: recorded no moves (nothing to undo)\n"; break;
        case ForwardChainState::AbandonedForwardInit:
            out << "  newest run: abandoned initialization (nothing to undo)\n"; break;
        case ForwardChainState::Blocked:
            out << "  newest run: BLOCKED - " << plan.fold.detail << "\n";
            out << "  undo is not available; the evidence is read-only\n";
            return out.str();
    }
    if (plan.fold.durability_caveat) {
        out << "  durability caveat: parts of the journal were not flush-confirmed\n";
    }
    if (plan.fold.candidates.empty()) {
        out << "  nothing to undo\n";
        return out.str();
    }
    out << "  undo candidates:\n";
    for (const auto& c : plan.fold.candidates) {
        out << "    move " << c.move_id << ": " << c.source_name << " (from "
            << category_name(c.dest_cat) << " as " << c.dest_name << ") - ";
        switch (c.classification) {
            case UndoCandidateClass::Undoable:        out << "undoable"; break;
            case UndoCandidateClass::AlreadyReversed: out << "already reversed"; break;
            case UndoCandidateClass::Conflict:        out << "CONFLICT (source slot refilled)"; break;
            case UndoCandidateClass::Missing:         out << "MISSING"; break;
            case UndoCandidateClass::Replaced:        out << "REPLACED (identity mismatch)"; break;
            case UndoCandidateClass::Ambiguous:       out << "AMBIGUOUS"; break;
            case UndoCandidateClass::Blocked:         out << "BLOCKED"; break;
            case UndoCandidateClass::Corrupt:         out << "CORRUPT"; break;
        }
        out << " - " << c.detail << "\n";
    }
    return out.str();
}

std::string render_undo_execution(const UndoPlanResult& plan,
                                  const UndoExecutionResult& result) {
    std::ostringstream out;
    out << "UNDO EXECUTION (run " << plan.root_run_seq << ")\n";
    if (result.aborted) {
        out << "  refused before any change: " << result.abort_reason << "\n";
        return out.str();
    }
    out << "  segment: " << (result.segment_active ? "created" : "not created") << "\n";
    out << "  restored: " << result.restored_count << " of " << result.eligible_count
        << " eligible\n";
    if (result.conflict_count != 0) {
        out << "  conflicts: " << result.conflict_count << "\n";
    }
    if (result.failed_count != 0) {
        out << "  failures: " << result.failed_count << "\n";
    }
    if (result.refused_count != 0) {
        out << "  refused (not journaled): " << result.refused_count << "\n";
    }
    for (const auto& r : result.records) {
        out << "    move " << r.move_id << ": " << r.source_name << " <- "
            << r.dest_name << " : " << r.outcome
            << (r.journaled ? "" : " (not journaled)") << "\n";
    }
    if (result.segment_stopped) {
        out << "  SEGMENT STOPPED: " << result.segment_error << "\n";
        out << "  the undo is partial; the durable state is classified per the "
               "journal contract\n";
    } else if (result.restored_count != result.eligible_count ||
               result.conflict_count != 0 || result.failed_count != 0) {
        out << "  partial undo completed\n";
    }
    return out.str();
}

} // namespace folderpilot
