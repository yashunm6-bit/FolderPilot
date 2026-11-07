// FolderPilot - immutable organization plan implementation.

#include "folderpilot/organization_plan.hpp"

namespace folderpilot {

std::size_t OrganizationPlan::unsafe_category_count() const {
    std::size_t count = 0;
    for (const CategoryStatus& status : categories_) {
        if (status.state == CategoryDirectoryState::Unsafe ||
            status.state == CategoryDirectoryState::Uninspectable) {
            ++count;
        }
    }
    return count;
}

std::size_t OrganizationPlan::existing_category_count() const {
    std::size_t count = 0;
    for (const CategoryStatus& status : categories_) {
        if (status.state == CategoryDirectoryState::Safe) {
            ++count;
        }
    }
    return count;
}

std::size_t OrganizationPlan::missing_category_count() const {
    std::size_t count = 0;
    for (const CategoryStatus& status : categories_) {
        if (status.state == CategoryDirectoryState::Missing) {
            ++count;
        }
    }
    return count;
}

std::size_t OrganizationPlan::collision_resolved_count() const {
    std::size_t count = 0;
    for (const PlannedMove& move : moves_) {
        if (move.source_name != move.destination_name) {
            ++count;
        }
    }
    return count;
}

} // namespace folderpilot
