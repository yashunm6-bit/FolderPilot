// Phase 4 unit tests: planning-time collision resolution (pure logic).

#include <doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "folderpilot/collision_resolver.hpp"

using namespace folderpilot;

TEST_CASE("destination name splitting preserves the full extension") {
    const auto ordinary = split_destination_name("report.pdf");
    CHECK(ordinary.stem == "report");
    CHECK(ordinary.extension == ".pdf");

    const auto compound = split_destination_name("backup.tar.gz");
    CHECK(compound.stem == "backup");
    CHECK(compound.extension == ".tar.gz");

    const auto compound2 = split_destination_name("site.Tar.Bz2");
    // Case-insensitive compound match: the WHOLE ".Tar.Bz2" is the extension
    // and is preserved with its original spelling.
    CHECK(compound2.stem == "site");
    CHECK(compound2.extension == ".Tar.Bz2");

    const auto extensionless = split_destination_name("README");
    CHECK(extensionless.stem == "README");
    CHECK(extensionless.extension.empty());

    const auto dotfile = split_destination_name(".gitignore");
    CHECK(dotfile.stem == ".gitignore"); // leading dot is not an extension
    CHECK(dotfile.extension.empty());

    const auto trailing = split_destination_name("file.");
    CHECK(trailing.stem == "file");
    CHECK(trailing.extension == ".");
}

TEST_CASE("suffix generation appends (n) before the extension") {
    const auto ordinary = split_destination_name("report.pdf");
    CHECK(suffixed_name(ordinary, 2) == "report (2).pdf");
    CHECK(suffixed_name(ordinary, 10) == "report (10).pdf");

    const auto compound = split_destination_name("backup.tar.gz");
    CHECK(suffixed_name(compound, 2) == "backup (2).tar.gz");

    const auto dotfile = split_destination_name(".gitignore");
    CHECK(suffixed_name(dotfile, 2) == ".gitignore (2)");

    const auto extensionless = split_destination_name("README");
    CHECK(suffixed_name(extensionless, 2) == "README (2)");
}

TEST_CASE("no collision keeps the natural name") {
    CollisionResolver resolver;
    resolver.reserve_natural("report.pdf");
    CHECK(resolver.allocate("report.pdf") == std::optional<std::string>{"report.pdf"});
}

TEST_CASE("existing identical filename produces a suffix") {
    CollisionResolver resolver;
    resolver.add_existing("report.pdf");
    resolver.reserve_natural("report.pdf");
    CHECK(resolver.allocate("report.pdf") == std::optional<std::string>{"report (2).pdf"});
}

TEST_CASE("existing case-variant filename produces a suffix") {
    CollisionResolver resolver;
    resolver.add_existing("Resume.PDF");
    resolver.reserve_natural("resume.pdf");
    CHECK(resolver.allocate("resume.pdf") == std::optional<std::string>{"resume (2).pdf"});
}

TEST_CASE("three sequential collisions produce (2), (3), (4)") {
    CollisionResolver resolver;
    resolver.add_existing("photo.jpg");
    for (int i = 0; i < 3; ++i) {
        resolver.reserve_natural("photo.jpg");
    }
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo (2).jpg"});
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo (3).jpg"});
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo (4).jpg"});
}

TEST_CASE("a generated suffix never steals an incoming natural name") {
    CollisionResolver resolver;
    resolver.add_existing("report.pdf");
    // Two incoming files: one named report.pdf, one NAMED "report (2).pdf".
    resolver.reserve_natural("report.pdf");
    resolver.reserve_natural("report (2).pdf");

    // The first file must SKIP the reserved "report (2).pdf" and take (3).
    CHECK(resolver.allocate("report.pdf") == std::optional<std::string>{"report (3).pdf"});
    // The second file keeps its own natural name.
    CHECK(resolver.allocate("report (2).pdf") == std::optional<std::string>{"report (2).pdf"});
}

TEST_CASE("two incoming names colliding case-insensitively both get destinations") {
    CollisionResolver resolver;
    resolver.reserve_natural("A.TXT");
    resolver.reserve_natural("a.txt");

    CHECK(resolver.allocate("A.TXT") == std::optional<std::string>{"A.TXT"});
    CHECK(resolver.allocate("a.txt") == std::optional<std::string>{"a (2).txt"});
}

TEST_CASE("unicode case-variant collisions are handled conservatively") {
    CollisionResolver resolver;
    resolver.add_existing("\xC3\x84nderungen.txt"); // Ä...
    resolver.reserve_natural("\xC3\xA4nderungen.txt"); // ä... (same name case-folded)
    CHECK(resolver.allocate("\xC3\xA4nderungen.txt") ==
          std::optional<std::string>{"\xC3\xA4nderungen (2).txt"});
}

TEST_CASE("dotfiles, extensionless and compound names collide with suffixes") {
    {
        CollisionResolver resolver;
        resolver.add_existing(".gitignore");
        resolver.reserve_natural(".gitignore");
        CHECK(resolver.allocate(".gitignore") == std::optional<std::string>{".gitignore (2)"});
    }
    {
        CollisionResolver resolver;
        resolver.add_existing("README");
        resolver.reserve_natural("README");
        CHECK(resolver.allocate("README") == std::optional<std::string>{"README (2)"});
    }
    {
        CollisionResolver resolver;
        resolver.add_existing("backup.tar.gz");
        resolver.reserve_natural("backup.tar.gz");
        CHECK(resolver.allocate("backup.tar.gz") ==
              std::optional<std::string>{"backup (2).tar.gz"});
    }
}

TEST_CASE("suffix attempts are bounded and report exhaustion") {
    CollisionResolver resolver{2}; // only " (2)" and " (3)" allowed
    for (int i = 0; i < 4; ++i) {
        resolver.reserve_natural("photo.jpg");
    }
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo.jpg"});
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo (2).jpg"});
    CHECK(resolver.allocate("photo.jpg") == std::optional<std::string>{"photo (3).jpg"});
    CHECK(resolver.allocate("photo.jpg") == std::nullopt); // CollisionExhausted
    CHECK(resolver.max_suffix_attempts() == 2);
}

TEST_CASE("resolution is deterministic across repeated runs") {
    for (int run = 0; run < 2; ++run) {
        CollisionResolver resolver;
        resolver.add_existing("report.pdf");
    for (const char* n : {"report.pdf", "report.pdf", "report (2).pdf"}) {
        resolver.reserve_natural(n);
    }
        CHECK(resolver.allocate("report.pdf") == std::optional<std::string>{"report (3).pdf"});
        CHECK(resolver.allocate("report.pdf") == std::optional<std::string>{"report (4).pdf"});
        CHECK(resolver.allocate("report (2).pdf") == std::optional<std::string>{"report (2).pdf"});
    }
}
