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
    CHECK(run.output.find("resume.pdf -> Documents/resume.pdf: Moved") != std::string::npos);
    CHECK(run.output.find("notes.txt -> Other/notes.txt: Moved") != std::string::npos);
    CHECK(run.output.find("backup.zip -> Archives/backup.zip: Moved") != std::string::npos);
    CHECK(run.output.find("Execution completed successfully.") != std::string::npos);

    // Full structure verification.
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume.pdf", "resume-content"));
    CHECK(file_content_matches(dir.path() / L"Other" / L"notes.txt", "notes-content"));
    CHECK(file_content_matches(dir.path() / L"Archives" / L"backup.zip", "zip-content"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"resume.pdf"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"notes.txt"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"backup.zip"));
}

TEST_CASE("CLI --apply reports partial failures with exit code 3") {
    if (!root_relative_rename_supported()) {
        MESSAGE(kMovementSkipReason);
        return;
    }
    SandboxDir dir{"cli5-partial"};
    REQUIRE(fixture::create_file(dir.path(), L"good.txt", "good"));
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "plan-me"));

    // First run moves everything (on supported builds).
    const auto first = run_folderpilot_cli({wide_path(dir), L"--apply"});
    if (first.exit_code != 0) {
        MESSAGE(kMovementSkipReason);
        return;
    }
    REQUIRE(std::filesystem::exists(dir.path() / L"Documents" / L"resume.pdf"));

    // Second run: create a NEW resume.pdf (will be planned as
    // "resume (2).pdf"), then inject a late conflict under that name.
    REQUIRE(fixture::create_file(dir.path(), L"resume.pdf", "second"));
    const auto dry = run_folderpilot_cli({wide_path(dir)});
    CHECK(dry.exit_code == 0);
    CHECK(dry.output.find("resume (2).pdf") != std::string::npos);
    REQUIRE(fixture::create_file(dir.path() / L"Documents", L"resume (2).pdf", "late-conflict"));

    const auto second = run_folderpilot_cli({wide_path(dir), L"--apply"});
    CHECK(second.exit_code == 3);
    CHECK(second.output.find("resume.pdf -> Documents/resume (2).pdf: Conflict") != std::string::npos);
    CHECK(second.output.find("Execution completed with partial failures.") != std::string::npos);
    // Source and conflicting destination both preserved.
    CHECK(file_content_matches(dir.path() / L"resume.pdf", "second"));
    CHECK(file_content_matches(dir.path() / L"Documents" / L"resume (2).pdf", "late-conflict"));
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
