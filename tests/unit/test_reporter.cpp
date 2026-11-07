// Phase 4 tests: dry-run reporter (consumes real plans built on sandbox
// fixtures; the reporter itself is pure).

#include <doctest.h>

#include <string>

#include "../phase3_fixture.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/reporter.hpp"

using namespace folderpilot;

TEST_CASE("reporter renders the dry-run preview for a simple plan") {
    fixture::SandboxDir dir{"report-simple"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "r"));
    REQUIRE(fixture::create_file(dir.path(), L"photo.jpg", "p"));
    REQUIRE(fixture::create_file(dir.path(), L"backup.tar.gz", "b"));
    REQUIRE(fixture::create_file(dir.path(), L"main.cpp", "c"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());

    const std::string report = render_dry_run_report(plan);

    CHECK(report.find("FOLDERPILOT - DRY RUN") != std::string::npos);
    CHECK(report.find("Target:") != std::string::npos);
    CHECK(report.find(dir.u8()) != std::string::npos);
    CHECK(report.find("resume.pdf -> Documents/resume.pdf") != std::string::npos);
    CHECK(report.find("photo.jpg -> Images/photo.jpg") != std::string::npos);
    CHECK(report.find("backup.tar.gz -> Archives/backup.tar.gz") != std::string::npos);
    CHECK(report.find("main.cpp -> Code/main.cpp") != std::string::npos);
    CHECK(report.find("Files scanned:             4") != std::string::npos);
    CHECK(report.find("Planned moves:             4") != std::string::npos);
    CHECK(report.find("NO FILES WERE MOVED.") != std::string::npos);
    CHECK(report.find("NO DIRECTORIES WERE CREATED.") != std::string::npos);
    CHECK(report.find("NO FILES WERE DELETED.") != std::string::npos);
    // Missing category folders are disclosed as future work, not as done.
    CHECK(report.find("Missing category folders:  7") != std::string::npos);
    CHECK(report.find("would be created during a later execution phase") != std::string::npos);
}

TEST_CASE("reporter distinguishes existing from missing category folders") {
    fixture::SandboxDir dir{"report-existing"};
    REQUIRE(fixture::create_file(dir.path(), L"photo.jpg", "p"));
    REQUIRE(fixture::create_directory(dir.path(), L"Images")); // exists

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());

    const std::string report = render_dry_run_report(plan);
    CHECK(report.find("Images (existing):") != std::string::npos);
    CHECK(report.find("Existing category folders: 1") != std::string::npos);
    CHECK(report.find("Missing category folders:  6") != std::string::npos);
}

TEST_CASE("reporter discloses collisions and skip reasons") {
    fixture::SandboxDir dir{"report-collision"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "new"));
    REQUIRE(fixture::create_directory(dir.path(), L"Documents"));
    REQUIRE(fixture::create_file(dir.path() / L"Documents", L"resume.pdf", "old"));
    REQUIRE(fixture::create_file(dir.path(), L"hidden.dat", "h"));
    REQUIRE(fixture::add_attributes(dir.path() / L"hidden.dat", FILE_ATTRIBUTE_HIDDEN));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());

    const std::string report = render_dry_run_report(plan);
    CHECK(report.find("resume.pdf -> Documents/resume (2).pdf") != std::string::npos);
    CHECK(report.find("(collision-resolved)") != std::string::npos);
    CHECK(report.find("Collision-resolved names:  1") != std::string::npos);
    CHECK(report.find("Skipped:") != std::string::npos);
    CHECK(report.find("hidden.dat") != std::string::npos);
}

TEST_CASE("reporter discloses unsafe categories without planning into them") {
    fixture::SandboxDir dir{"report-unsafe"};
    REQUIRE(fixture::create_file(dir.path(), L"doc.pdf", "d"));
    REQUIRE(fixture::create_directory(dir.path(), L"Documents"));
    REQUIRE(fixture::add_attributes(dir.path() / L"Documents", FILE_ATTRIBUTE_HIDDEN));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());

    const std::string report = render_dry_run_report(plan);
    CHECK(report.find("Documents (UNSAFE):") != std::string::npos);
    CHECK(report.find("hidden") != std::string::npos);
    CHECK(report.find("Unsafe categories:         1") != std::string::npos);
}
