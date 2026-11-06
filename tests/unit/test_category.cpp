// Phase 2 unit tests: Category type.

#include <doctest.h>

#include <string_view>

#include "folderpilot/category.hpp"

using namespace folderpilot;

TEST_CASE("all categories exist and resolve to a real name") {
    for (const Category category : all_categories) {
        CHECK(category_name(category) != "Unknown");
        CHECK(!category_name(category).empty());
    }
}

TEST_CASE("display names match the documented stable names") {
    CHECK(category_name(Category::Documents) == "Documents");
    CHECK(category_name(Category::Images) == "Images");
    CHECK(category_name(Category::Videos) == "Videos");
    CHECK(category_name(Category::Audio) == "Audio");
    CHECK(category_name(Category::Archives) == "Archives");
    CHECK(category_name(Category::Code) == "Code");
    CHECK(category_name(Category::Other) == "Other");
}

TEST_CASE("display names are unique (they become folder names)") {
    for (std::size_t i = 0; i < all_categories.size(); ++i) {
        for (std::size_t j = i + 1; j < all_categories.size(); ++j) {
            CHECK(category_name(all_categories[i]) != category_name(all_categories[j]));
        }
    }
}

TEST_CASE("invalid enum values are handled safely") {
    CHECK(category_name(static_cast<Category>(7)) == "Unknown");
    CHECK(category_name(static_cast<Category>(42)) == "Unknown");
    CHECK(category_name(static_cast<Category>(-1)) == "Unknown");
}
