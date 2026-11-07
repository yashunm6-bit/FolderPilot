// Phase 4 tests: organization planner (pure invariant checks plus sandboxed
// integration with real destinations).

#include <doctest.h>

#include <string>
#include <vector>

#include "../phase3_fixture.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"

using namespace folderpilot;

namespace {

PlannedMove make_move(const std::string& source, const std::string& destination,
                      Category category) {
    PlannedMove move;
    move.source_name = source;
    move.identity = platform::SourceIdentityEx{1, {}};
    move.source_category = category;
    move.destination_category = category;
    move.destination_name = destination;
    move.size_bytes = 1;
    move.last_write_time = 100;
    return move;
}

CategoryStatus make_status(Category category, CategoryDirectoryState state,
                           std::vector<std::string> existing = {}) {
    CategoryStatus status;
    status.category = category;
    status.state = state;
    status.existing_names = std::move(existing);
    return status;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure invariant validation (synthetic plans; no filesystem access).
// ---------------------------------------------------------------------------

TEST_CASE("plan invariants accept a consistent plan") {
    std::vector<PlannedMove> moves{
        make_move("resume.pdf", "resume.pdf", Category::Documents),
        make_move("photo.jpg", "photo (2).jpg", Category::Images),
    };
    std::vector<CategoryStatus> statuses{
        make_status(Category::Documents, CategoryDirectoryState::Missing),
        make_status(Category::Images, CategoryDirectoryState::Safe),
    };
    CHECK_FALSE(validate_plan_invariants(moves, statuses).has_value());
}

TEST_CASE("plan invariants reject duplicate destinations") {
    std::vector<PlannedMove> moves{
        make_move("a.pdf", "a (2).pdf", Category::Documents),
        make_move("b.pdf", "a (2).pdf", Category::Documents),
    };
    std::vector<CategoryStatus> statuses{
        make_status(Category::Documents, CategoryDirectoryState::Safe),
    };
    const auto violation = validate_plan_invariants(moves, statuses);
    REQUIRE(violation.has_value());
    CHECK(violation->find("duplicate") != std::string::npos);
}

TEST_CASE("plan invariants reject separators, traversal and empty names") {
    const std::string bad_names[] = {"a\\b.pdf", "a/b.pdf", "a:b.pdf", "..\\x.pdf", "..", ""};
    for (const std::string& bad : bad_names) {
        std::vector<PlannedMove> moves{make_move("src.pdf", bad, Category::Documents)};
        const auto violation = validate_plan_invariants(moves, {});
        REQUIRE(violation.has_value());
        CHECK(violation->find("single safe component") != std::string::npos);
    }
}

TEST_CASE("plan invariants reject category disagreement and unsafe categories") {
    {
        PlannedMove move = make_move("a.pdf", "a.pdf", Category::Documents);
        move.destination_category = Category::Images; // inconsistent
        const std::vector<PlannedMove> moves{move};
        const auto violation = validate_plan_invariants(moves, {});
        REQUIRE(violation.has_value());
        CHECK(violation->find("disagree") != std::string::npos);
    }
    {
        std::vector<PlannedMove> moves{make_move("a.pdf", "a.pdf", Category::Documents)};
        std::vector<CategoryStatus> statuses{
            make_status(Category::Documents, CategoryDirectoryState::Unsafe),
        };
        const auto violation = validate_plan_invariants(moves, statuses);
        REQUIRE(violation.has_value());
        CHECK(violation->find("unsafe category") != std::string::npos);
    }
    {
        std::vector<PlannedMove> moves{make_move("a.pdf", "a.pdf", Category::Documents)};
        std::vector<CategoryStatus> statuses{
            make_status(Category::Documents, CategoryDirectoryState::Uninspectable),
        };
        CHECK(validate_plan_invariants(moves, statuses).has_value());
    }
}

TEST_CASE("plan invariants reject empty source names") {
    std::vector<PlannedMove> moves{make_move("", "a.pdf", Category::Documents)};
    const auto violation = validate_plan_invariants(moves, {});
    REQUIRE(violation.has_value());
    CHECK(violation->find("empty source name") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Integration tests (sandboxed).
// ---------------------------------------------------------------------------

TEST_CASE("planner produces an empty but valid plan for an empty directory") {
    fixture::SandboxDir dir{"plan-empty"};
    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());
    CHECK(plan.planned_count() == 0);
    CHECK(plan.scanned_count() == 0);
    CHECK(plan.missing_category_count() == 7); // nothing exists yet
    CHECK(plan.existing_category_count() == 0);
}

TEST_CASE("planner plans one file into a missing category and preserves identity") {
    fixture::SandboxDir dir{"plan-one"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "resume-bytes"));
    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);

    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());
    REQUIRE(plan.planned_count() == 1);
    const PlannedMove& move = plan.moves()[0];
    CHECK(move.source_name == "resume.pdf");
    CHECK(move.destination_name == "resume.pdf");
    CHECK(move.source_category == Category::Documents);
    CHECK(move.destination_category == Category::Documents);
    CHECK(move.size_bytes == 12);
    CHECK(platform::identity_matches(move.identity, scan.files[0].identity));
    CHECK(plan.missing_category_count() == 7); // Documents is still missing
}

TEST_CASE("planner groups files into all seven categories") {
    fixture::SandboxDir dir{"plan-seven"};
    REQUIRE(fixture::create_file(dir.path(), L"a.pdf", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"b.jpg", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"c.mp4", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"d.mp3", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"e.zip", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"f.cpp", "x"));
    REQUIRE(fixture::create_file(dir.path(), L"mystery.bin", "x"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());
    CHECK(plan.planned_count() == 7);
    // Six named categories + Other (mystery.bin).
    std::vector<std::string> section_names;
    for (const PlannedMove& move : plan.moves()) {
        section_names.push_back(std::string{category_name(move.destination_category)});
    }
    for (const char* expected :
         {"Documents", "Images", "Videos", "Audio", "Archives", "Code", "Other"}) {
        bool found = false;
        for (const std::string& section : section_names) {
            if (section == expected) {
                found = true;
            }
        }
        const std::string message = std::string{"missing category section: "} + expected;
        CHECK_MESSAGE(found, message);
    }
}

TEST_CASE("planner resolves collisions against an existing category snapshot") {
    fixture::SandboxDir dir{"plan-collision"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "incoming"));
    REQUIRE(fixture::create_directory(dir.path(), L"Documents"));
    // A DIFFERENT resume.pdf already lives in the destination.
    REQUIRE(fixture::create_file(dir.path() / L"Documents", L"resume.pdf", "existing"));
    // A hidden entry also occupies a name that matters.
    REQUIRE(fixture::create_file(dir.path(), L"hidden.txt", "h"));
    REQUIRE(fixture::add_attributes(dir.path() / L"hidden.txt", FILE_ATTRIBUTE_HIDDEN));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());
    REQUIRE(plan.planned_count() == 1); // hidden.txt is a scan-phase skip
    CHECK(plan.moves()[0].destination_name == "resume (2).pdf");
    CHECK(plan.existing_category_count() == 1);
    CHECK(plan.skipped_count() >= 1);
}

TEST_CASE("unsafe categories receive zero planned moves; safe categories continue") {
    fixture::SandboxDir dir{"plan-unsafe"};
    REQUIRE(fixture::create_file(dir.path(), L"doc.pdf", "d"));
    REQUIRE(fixture::create_file(dir.path(), L"pic.jpg", "p"));
    // Documents exists but is HIDDEN -> unsafe for planning.
    REQUIRE(fixture::create_directory(dir.path(), L"Documents"));
    REQUIRE(fixture::add_attributes(dir.path() / L"Documents", FILE_ATTRIBUTE_HIDDEN));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    const auto plan = build_plan(guard.root, scan);
    REQUIRE(plan.valid());
    // Images (missing, therefore plannable) got its move.
    REQUIRE(plan.planned_count() == 1);
    CHECK(plan.moves()[0].destination_category == Category::Images);
    // Documents is unsafe; its candidate is skipped with a structured reason.
    CHECK(plan.unsafe_category_count() == 1);
    bool unsafe_skip = false;
    for (const SkippedEntry& entry : plan.skipped()) {
        if (entry.name_utf8 == "doc.pdf" && entry.reason == SkipReason::UnsafeCategory) {
            unsafe_skip = true;
        }
    }
    CHECK(unsafe_skip);
}

TEST_CASE("planner fails closed when the root changed between scan and planning") {
    fixture::SandboxDir dir{"plan-mismatch"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "r"));
    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    // Simulate a root identity change after the scan.
    scan.root.identity = platform::SourceIdentityEx{987654321, {}};

    const auto plan = build_plan(guard.root, scan);
    CHECK_FALSE(plan.valid());
    CHECK(plan.error().find("root identity") != std::string::npos);
}

TEST_CASE("planner fails closed for an invalid scan result") {
    fixture::SandboxDir dir{"plan-badscan"};
    ScanResult failed;
    failed.enumeration = EnumerationStatus::Failed;
    failed.error = "injected failure";

    const auto plan = build_plan(validate_target(dir.u8()).root, failed);
    CHECK_FALSE(plan.valid());
    CHECK(plan.error().find("scan failed") != std::string::npos);
}

TEST_CASE("planning is deterministic across repeated runs") {
    fixture::SandboxDir dir{"plan-deterministic"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "1"));
    REQUIRE(fixture::create_file(dir.path(), L"Resume.pdf", "2"));
    REQUIRE(fixture::create_directory(dir.path(), L"Documents"));
    REQUIRE(fixture::create_file(dir.path() / L"Documents", L"resume.pdf", "old"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto scan = scan_directory(guard.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);

    const auto first = build_plan(guard.root, scan);
    const auto second = build_plan(guard.root, scan);
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    REQUIRE(first.planned_count() == second.planned_count());
    for (std::size_t i = 0; i < first.planned_count(); ++i) {
        CHECK(first.moves()[i].source_name == second.moves()[i].source_name);
        CHECK(first.moves()[i].destination_name == second.moves()[i].destination_name);
    }
}
