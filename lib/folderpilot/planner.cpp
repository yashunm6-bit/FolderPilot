// FolderPilot - organization planner implementation.

#include "folderpilot/planner.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

#include "folderpilot/classifier.hpp"
#include "folderpilot/collision_resolver.hpp"
#include "native.hpp"

namespace folderpilot {

namespace {

struct CiLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return platform::compare_ordinal_ci(a, b) < 0;
    }
};

// A destination name must be a single safe filename component - the same
// defensive rule the platform layer applies to source opens.
bool is_safe_component(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

} // namespace

std::optional<std::string> validate_plan_invariants(
    std::span<const PlannedMove> moves,
    std::span<const CategoryStatus> categories) {

    std::set<std::string, CiLess> destinations;
    for (const PlannedMove& move : moves) {
        if (move.source_name.empty()) {
            return std::string{"planned move has an empty source name"};
        }
        if (!is_safe_component(move.destination_name)) {
            return "destination name is not a single safe component: " + move.destination_name;
        }
        if (move.source_category != move.destination_category) {
            return "source and destination category disagree for " + move.source_name;
        }
        if (!destinations.insert(move.destination_name).second) {
            return "duplicate planned destination: " + move.destination_name;
        }

        const auto status = std::find_if(
            categories.begin(), categories.end(), [&move](const CategoryStatus& s) {
                return s.category == move.destination_category;
            });
        if (status == categories.end()) {
            return "no category status recorded for " + move.destination_name;
        }
        if (status->state == CategoryDirectoryState::Unsafe ||
            status->state == CategoryDirectoryState::Uninspectable) {
            return "move targets an unsafe category: " + move.source_name;
        }
        if (status->state == CategoryDirectoryState::Safe) {
            for (const std::string& existing : status->existing_names) {
                if (platform::compare_ordinal_ci(existing, move.destination_name) == 0) {
                    return "destination conflicts with an existing entry: " +
                           move.destination_name;
                }
            }
        }
    }
    return std::nullopt;
}

OrganizationPlan build_plan(const platform::RootDirectory& root,
                            const ScanResult& scan,
                            const PlanOptions& options) {
    OrganizationPlan plan;

    // ---- 1. Validate the inputs (fail closed on anything unresolved). ----
    if (!root.valid()) {
        plan.error_ = "root handle is not valid";
        return plan;
    }
    if (scan.enumeration != EnumerationStatus::Ok) {
        plan.error_ = "scan failed: " + scan.error;
        return plan;
    }
    if (!platform::identity_matches(root.identity, scan.root.identity) ||
        platform::compare_ordinal_ci(root.resolved_path_utf8, scan.root.resolved_path) != 0) {
        plan.error_ = "root identity changed between scanning and planning";
        return plan;
    }

    plan.root_ = scan.root;
    plan.scanned_count_ = scan.files.size();

    // ---- 2. Inspect every destination category (read-only, root-relative).
    for (const Category category : all_categories) {
        const auto inspection =
            platform::inspect_category_directory(root.get(), category_name(category));
        CategoryStatus status;
        status.category = category;
        switch (inspection.status) {
            case platform::CategoryDirStatus::Safe:
                status.state = CategoryDirectoryState::Safe;
                status.existing_names = std::move(inspection.existing_names);
                status.directory_identity = inspection.directory_identity;
                status.has_directory_identity = inspection.has_directory_identity;
                break;
            case platform::CategoryDirStatus::Missing:
                status.state = CategoryDirectoryState::Missing;
                status.detail = inspection.detail;
                break;
            case platform::CategoryDirStatus::Unsafe:
                status.state = CategoryDirectoryState::Unsafe;
                status.detail = inspection.detail;
                break;
            case platform::CategoryDirStatus::Uninspectable:
                status.state = CategoryDirectoryState::Uninspectable;
                status.detail = inspection.detail;
                break;
        }
        plan.categories_.push_back(std::move(status));
    }

    // ---- 3. Classify and group candidates, preserving scan order.
    std::array<std::vector<const ScannedFile*>, 7> grouped{};
    for (const ScannedFile& file : scan.files) {
        const Category category = classify_filename(file.name_utf8);
        grouped[static_cast<std::size_t>(category)].push_back(&file);
    }

    // ---- 4. Plan per category: collisions are resolved entirely here.
    for (std::size_t index = 0; index < grouped.size(); ++index) {
        const Category category = static_cast<Category>(index);
        const CategoryStatus& status = plan.categories_[index];
        const auto& candidates = grouped[index];
        if (candidates.empty()) {
            continue;
        }

        if (status.state == CategoryDirectoryState::Unsafe) {
            for (const ScannedFile* file : candidates) {
                plan.skipped_.push_back(SkippedEntry{
                    file->name_utf8, SkipReason::UnsafeCategory,
                    "destination category is unsafe: " + status.detail, 0});
            }
            continue;
        }
        if (status.state == CategoryDirectoryState::Uninspectable) {
            for (const ScannedFile* file : candidates) {
                plan.skipped_.push_back(SkippedEntry{
                    file->name_utf8, SkipReason::CategoryUninspectable,
                    "destination category could not be inspected: " + status.detail, 0});
            }
            continue;
        }

        // Safe or Missing: resolve collisions. Missing categories have no
        // existing entries yet - the directory will be created in a later
        // execution phase, and the Reporter discloses that.
        CollisionResolver resolver{options.max_collision_suffixes};
        if (status.state == CategoryDirectoryState::Safe) {
            for (const std::string& existing : status.existing_names) {
                resolver.add_existing(existing);
            }
        }
        for (const ScannedFile* file : candidates) {
            resolver.reserve_natural(file->name_utf8);
        }
        for (const ScannedFile* file : candidates) {
            const auto destination = resolver.allocate(file->name_utf8);
            if (!destination.has_value()) {
                plan.skipped_.push_back(SkippedEntry{
                    file->name_utf8, SkipReason::CollisionExhausted,
                    "no free destination name within " +
                        std::to_string(options.max_collision_suffixes) + " suffix attempts",
                    0});
                continue;
            }
            PlannedMove move;
            move.source_name = file->name_utf8;
            move.identity = file->identity;
            move.source_category = category;
            move.destination_category = category;
            move.destination_name = *destination;
            move.size_bytes = file->size_bytes;
            move.last_write_time = file->last_write_time;
            plan.moves_.push_back(std::move(move));
        }
    }

    // ---- 5. Carry scan-phase skips into the plan's skip list.
    plan.skipped_.insert(plan.skipped_.end(), scan.skipped.begin(), scan.skipped.end());

    // ---- 6. Validate the plan invariants before publishing.
    const auto violation =
        validate_plan_invariants(plan.moves_, plan.categories_);
    if (violation.has_value()) {
        plan.moves_.clear();
        plan.skipped_.clear();
        plan.error_ = "plan invariant violated: " + *violation;
        return plan; // fail closed - no partial plan is ever published
    }

    plan.valid_ = true;
    return plan;
}

} // namespace folderpilot
