// FolderPilot - core category type.
//
// The seven categories are user-visible folder names. Their display names
// must remain stable across releases, because users will see them as the
// folders FolderPilot creates.

#pragma once

#include <array>
#include <string_view>

namespace folderpilot {

// The fixed set of destination categories. Kept independent of the
// classification logic: rules and classifier may change, these names may not.
enum class Category {
    Documents,
    Images,
    Videos,
    Audio,
    Archives,
    Code,
    Other,
};

// Stable, user-visible folder name for a category.
// Returns "Unknown" for values outside the enum's valid range, so that an
// invalid enum can never produce a bogus folder name.
std::string_view category_name(Category category);

// All categories in declaration order. Useful for exhaustive iteration in
// tests and (later) reporting without hard-coding the count everywhere.
inline constexpr std::array<Category, 7> all_categories{
    Category::Documents, Category::Images,   Category::Videos,
    Category::Audio,     Category::Archives, Category::Code,
    Category::Other,
};

} // namespace folderpilot
