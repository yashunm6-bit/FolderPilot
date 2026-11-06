// Phase 2 unit tests: extension rules table.

#include <doctest.h>

#include <array>
#include <set>
#include <string>

#include "../test_data.hpp"
#include "folderpilot/category.hpp"
#include "folderpilot/rules.hpp"

using namespace folderpilot;
namespace data = folderpilot_test_data;

TEST_CASE("every documented extension maps to its intended category") {
    for (const std::string_view ext : data::documents) {
        CHECK(lookup_extension(ext) == Category::Documents);
    }
    for (const std::string_view ext : data::images) {
        CHECK(lookup_extension(ext) == Category::Images);
    }
    for (const std::string_view ext : data::videos) {
        CHECK(lookup_extension(ext) == Category::Videos);
    }
    for (const std::string_view ext : data::audio) {
        CHECK(lookup_extension(ext) == Category::Audio);
    }
    for (const std::string_view ext : data::archives) {
        CHECK(lookup_extension(ext) == Category::Archives);
    }
    for (const std::string_view ext : data::code) {
        CHECK(lookup_extension(ext) == Category::Code);
    }
}

TEST_CASE("lookup is ASCII case-insensitive") {
    CHECK(lookup_extension("PDF") == Category::Documents);
    CHECK(lookup_extension("JpG") == Category::Images);
    CHECK(lookup_extension("MkV") == Category::Videos);
    CHECK(lookup_extension("FLAC") == Category::Audio);
    CHECK(lookup_extension("TgZ") == Category::Archives);
    CHECK(lookup_extension("JsOn") == Category::Code);
}

TEST_CASE("rule table is sorted strictly increasing, therefore duplicate-free") {
    const auto rules = extension_rules();
    REQUIRE(rules.size() > 1);
    for (std::size_t i = 1; i < rules.size(); ++i) {
        CHECK(rules[i - 1].extension < rules[i].extension);
    }
}

TEST_CASE("rule table size exactly matches the documented rule count") {
    CHECK(extension_rules().size() == data::total_documented_rules);
    CHECK(data::total_documented_rules == 89);
}

TEST_CASE("every rules-bearing category is represented; Other is the fallback") {
    std::set<std::string> categories_seen;
    for (const ExtensionRule& rule : extension_rules()) {
        categories_seen.insert(std::string{category_name(rule.category)});
    }
    CHECK(categories_seen.count("Documents") == 1);
    CHECK(categories_seen.count("Images") == 1);
    CHECK(categories_seen.count("Videos") == 1);
    CHECK(categories_seen.count("Audio") == 1);
    CHECK(categories_seen.count("Archives") == 1);
    CHECK(categories_seen.count("Code") == 1);
    CHECK(categories_seen.count("Other") == 0); // fallback has no rules
}

TEST_CASE("unknown extensions are not in the table") {
    for (const std::string_view unknown : {"exe", "dmg", "apk", "bin", "xyz123", ""}) {
        CHECK_FALSE(lookup_extension(unknown).has_value());
    }
    const auto rules = extension_rules();
    for (const ExtensionRule& rule : rules) {
        CHECK(rule.extension != "exe");
        CHECK(rule.extension != "dmg");
        CHECK(rule.extension != "apk");
    }
}

TEST_CASE("compound archive suffixes are exposed") {
    const auto suffixes = compound_archive_suffixes();
    REQUIRE(suffixes.size() == 3);
    CHECK(suffixes[0] == ".tar.gz");
    CHECK(suffixes[1] == ".tar.bz2");
    CHECK(suffixes[2] == ".tar.xz");
}
