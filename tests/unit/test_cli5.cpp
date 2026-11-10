// Phase 5 tests: CLI --apply integration (sandboxed synthetic fixtures only).

#include <doctest.h>

#include <string>

#include "../phase3_fixture.hpp"
#include "text.hpp"

using namespace fixture;

namespace {

std::wstring wide_path(const SandboxDir& dir) {
    return folderpilot::platform::to_utf16(dir.u8()).value;
}

} // namespace

TEST_CASE("CLI --apply moves files and leaves the root clean of sources") {
    if (!root_relative_rename_supported()) {
        MESSAGE(kMovementSkipReason);
        return;
    }
    SandboxDir dir{"cli5-apply"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "resume-content"));
    REQUIRE(fixture::create_file(dir.path(), L"notes.txt", "notes-content"));
    REQUIRE(fixture::create_directory(dir.path(), L"Archives")); // pre-existing category
    REQUIRE(fixture::create_file(dir.path(), L"backup.zip", "zip-content"));

    const auto run = run_folderpilot_cli({wide_path(dir), L"--apply"});
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("EXECUTION REPORT") != std::string::npos);
    // Execution-report format: category header + bare destination name.
    // (.txt maps to Documents per the classifier rules - never Other.)
    CHECK(run.output.find("Documents:") != std::string::npos);
    CHECK(run.output.find("resume.pdf -> resume.pdf: Moved") != std::string::npos);
    CHECK(run.output.find("notes.txt -> notes.txt: Moved") != std::string::npos);
    CHECK(run.output.find("Archives:") != std::string::npos);
    CHECK(run.output.find("backup.zip -> backup.zip: Moved") != std::string::npos);
    CHECK(run.output.find("Execution completed successfully.") != std::string::npos);

    // Full structure verification.
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume.pdf", "resume-content"));
    CHECK(file_content_matches(dir.path() / L"Documents" / L"notes.txt", "notes-content"));
    CHECK(file_content_matches(dir.path() / L"Archives" / L"backup.zip", "zip-content"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"resume.pdf"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"notes.txt"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"backup.zip"));
}

TEST_CASE("CLI --apply replans on every invocation: an injected conflict is re-reserved, never overwritten") {
    // Architecture note (Phase 5.1.5): a per-move Conflict outcome cannot be
    // produced deterministically from OUTSIDE a single CLI invocation. The
    // ratified plan->validate->execute seam runs planning and execution
    // back-to-back inside ONE process with no external intervention point;
    // any externally injected change therefore lands in the NEXT invocation,
    // which replans from scratch and re-reserves destination names. The
    // original version of this test assumed plan reuse across invocations.
    // This test asserts the real cross-invocation contract: fresh
    // reservation, and nothing is ever overwritten. The Conflict outcome,
    // both-sides preservation, and per-move isolation are covered at the
    // executor level in test_executor.cpp (conflict, category deleted,
    // category replaced, source replaced/vanished, locked source).
    if (!root_relative_rename_supported()) {
        MESSAGE(kMovementSkipReason);
        return;
    }
    SandboxDir dir{"cli5-replan"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "first"));

    const auto first = run_folderpilot_cli({wide_path(dir), L"--apply"});
    REQUIRE(first.exit_code == 0);
    REQUIRE(std::filesystem::exists(dir.path() / L"Documents" / L"resume.pdf"));

    // Between the two invocations: a new resume.pdf in the root (it would
    // collide with the moved one) AND an injected "resume (2).pdf" in the
    // destination (blocking the next reservation slot).
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "second"));
    REQUIRE(fixture::create_file(dir.path() / L"Documents", L"resume (2).pdf", "injected"));

    const auto second = run_folderpilot_cli({wide_path(dir), L"--apply"});
    CHECK(second.exit_code == 0);
    // Fresh reservation: "resume.pdf" and "resume (2).pdf" are both taken in
    // the destination, so the planner reserves "resume (3).pdf" and the
    // executor moves the file there - overwriting nothing.
    CHECK(second.output.find("resume.pdf -> resume (3).pdf: Moved") != std::string::npos);
    CHECK(std::filesystem::exists(dir.path() / L"Documents" / L"resume (3).pdf"));
    // All three files exist with their own content; nothing was overwritten.
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume.pdf", "first"));
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume (2).pdf", "injected"));
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume (3).pdf", "second"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"resume.pdf"));
}

TEST_CASE("CLI dry-run still changes nothing after Phase 5") {
    SandboxDir dir{"cli5-dry"};
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "r"));

    const auto before = take_snapshot(dir.path());
    const auto run = run_folderpilot_cli({wide_path(dir)});
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("FOLDERPILOT - DRY RUN") != std::string::npos);
    const auto after = take_snapshot(dir.path());
    CHECK(snapshots_equal(before, after));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"Documents"));
}
