// FolderPilot - organization planner.
//
// Consumes a validated ScanResult plus the retained root handle, classifies
// every scanned file, inspects the destination category directories through
// the platform layer (read-only, root-relative), resolves collisions during
// planning, validates the result against the plan invariants, and publishes
// an immutable OrganizationPlan. The planner never mutates anything.

#pragma once

#include <optional>
#include <span>
#include <string>

#include "folderpilot/organization_plan.hpp"
#include "folderpilot/scanner.hpp"

namespace folderpilot {

struct PlanOptions {
    // Maximum number of " (n)" suffix attempts per source file.
    std::size_t max_collision_suffixes = 100;
};

// Internal invariant checker, exposed for direct unit testing. Validates:
//   * non-empty, single-component destination names (no separators,
//     traversal, or "." / ".."),
//   * source and destination category agree,
//   * destination names are pairwise distinct (ordinal case-insensitive),
//   * no destination conflicts with a known existing snapshot entry,
//   * no move targets an unsafe or uninspectable category,
//   * every source name is non-empty (identity presence is guaranteed by
//     the scanner - files without identity are never scanned).
// Returns nullopt when the plan is consistent.
std::optional<std::string> validate_plan_invariants(
    std::span<const PlannedMove> moves,
    std::span<const CategoryStatus> categories);

// Builds the immutable plan. Never mutates the filesystem. If the scan is
// invalid, the root changed between scan and planning, or an internal
// invariant fails, the returned plan is invalid() with an error message
// (fail closed) instead of a partially valid structure.
OrganizationPlan build_plan(const platform::RootDirectory& root,
                            const ScanResult& scan,
                            const PlanOptions& options = {});

} // namespace folderpilot
