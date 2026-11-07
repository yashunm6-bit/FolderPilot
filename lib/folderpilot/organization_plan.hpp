// FolderPilot - immutable organization plan.
//
// The plan is a VALUE OBJECT built once by the Planner and consumed
// read-only by the Reporter (Phase 4) and the Executor (Phase 5). It has
// private storage, no mutators, and read-only accessors: after construction
// nothing - not the Reporter, not the future Executor - can change a
// destination name or any other part of the plan.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "folderpilot/category.hpp"
#include "folderpilot/scanner.hpp"

namespace folderpilot {

struct PlanOptions; // defined in planner.hpp

class Planner; // constructs the plan (friend)

// One fully resolved planned operation. The destination filename is final:
// it was decided during planning, collisions included, and is never changed
// downstream.
struct PlannedMove {
    std::string source_name;              // original spelling, preserved
    platform::SourceIdentity identity;    // canonical identity from the scan
    Category source_category{};           // classification result
    Category destination_category{};      // always equal to source_category
    std::string destination_name;         // final name inside the category folder
    unsigned long long size_bytes = 0;
    long long last_write_time = 0;        // Windows FILETIME (100ns units)
};

// Outcome of inspecting one destination category directory.
enum class CategoryDirectoryState {
    Safe,          // genuine directory, clean attributes, snapshot taken
    Missing,       // confirmed absent - would need creation in a later phase
    Unsafe,        // reparse/hidden/system leaf, or name exists as a file
    Uninspectable, // inspection failed - no planning into it
};

struct CategoryStatus {
    Category category{};
    CategoryDirectoryState state = CategoryDirectoryState::Uninspectable;
    std::string detail;                       // reason for Unsafe/Uninspectable
    std::vector<std::string> existing_names;  // destination snapshot; Safe only
};

class OrganizationPlan {
public:
    // Read-only accessors. There are deliberately no mutators.
    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const RootInfo& root() const { return root_; }
    std::span<const PlannedMove> moves() const { return moves_; }
    std::span<const SkippedEntry> skipped() const { return skipped_; }
    std::span<const CategoryStatus> categories() const { return categories_; }

    std::size_t scanned_count() const { return scanned_count_; }
    std::size_t planned_count() const { return moves_.size(); }
    std::size_t skipped_count() const { return skipped_.size(); }
    std::size_t unsafe_category_count() const;
    std::size_t existing_category_count() const;
    std::size_t missing_category_count() const;
    // Moves whose destination name differs from their source name.
    std::size_t collision_resolved_count() const;

private:
    friend OrganizationPlan build_plan(const platform::RootDirectory&,
                                       const ScanResult&, const PlanOptions&);

    OrganizationPlan() = default;

    bool valid_ = false;
    std::string error_;
    RootInfo root_{};
    std::vector<PlannedMove> moves_;
    std::vector<SkippedEntry> skipped_;
    std::vector<CategoryStatus> categories_;
    std::size_t scanned_count_ = 0;
};

} // namespace folderpilot
