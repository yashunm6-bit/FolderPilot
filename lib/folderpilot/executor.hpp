// FolderPilot - safe file executor.
//
// Consumes the EXACT immutable OrganizationPlan produced by the planner,
// revalidates everything against the live filesystem through the platform
// layer, and moves files with OS-enforced no-overwrite semantics
// (SetFileInformationByHandle(FileRenameInfo), ReplaceIfExists = FALSE,
// RootDirectory = validated destination handle, relative name from the plan).
//
// Guarantees (exactly as implemented - no stronger claims):
//   * per-file movement is atomic; the batch is NOT transactional,
//   * a failure on one file never aborts independent moves,
//   * an existing destination is never overwritten (the OS rejects it),
//   * the executor never invents destination names, never re-plans,
//     never retries, never deletes, never rolls back.

#pragma once

#include <string>
#include <vector>

#include "folderpilot/organization_plan.hpp"

namespace folderpilot {

// Structured per-move outcomes (no ambiguous strings as identifiers).
enum class MoveOutcome {
    Moved,
    Conflict,          // destination appeared after planning; source untouched
    SourceMissing,     // source (or an ancestor) is gone
    SourceReplaced,    // a different file/directory/link wears the source name
    SourceUnsafe,      // source became hidden or system
    Locked,            // source held by an incompatible existing handle
    AccessDenied,      // the OS refused the move
    DestinationUnsafe, // destination category is unsafe for execution
    DestinationChanged,// destination category replaced/deleted/appeared after planning
    CrossVolume,       // would cross a volume boundary (structurally impossible
                       // for root-relative renames; reported as a defect)
    DestinationInvalid,// destination name or handle failed validation
    SourceInvalid,     // source identity could not be reliably established
    Failed,            // anything else, reported with a native code
};

struct MoveRecord {
    std::string source_name;
    Category destination_category{};
    std::string destination_name;
    MoveOutcome outcome{};
    std::string detail;
    unsigned long native_code = 0;
};

struct ExecutionResult {
    bool aborted = false;              // batch-level abort (root/plan rejection)
    std::string abort_reason;
    std::size_t planned_count = 0;
    std::size_t moved_count = 0;
    std::size_t conflict_count = 0;
    std::size_t skipped_count = 0;     // Missing/Replaced/Unsafe/Changed outcomes
    std::size_t failed_count = 0;      // Locked/AccessDenied/CrossVolume/Failed/Invalid
    std::vector<std::string> created_directories;  // category dirs created now
    std::vector<MoveRecord> records;

    // Overall completion state for the CLI/Reporter.
    bool all_moved() const { return !aborted && moved_count == planned_count && failed_count == 0 && conflict_count == 0 && skipped_count == 0; }
};

// Executes the plan. `requested_path` is re-bootstrapped through the
// approved PathGuard mechanism; the fresh root identity, resolved path and
// volume GUID must match the plan, otherwise the whole execution aborts
// before any mutation (RootChanged). All `--apply` invocations must target
// synthetic sandbox fixtures during development.
ExecutionResult execute_plan(const OrganizationPlan& plan,
                             std::string_view requested_path);

} // namespace folderpilot
