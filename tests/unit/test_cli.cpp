// Phase 4 tests: read-only CLI integration (spawns the real executable).

#include <doctest.h>

#include <string>

#include "../phase3_fixture.hpp"
#include "text.hpp"

using namespace fixture;

namespace {

// The CLI takes a single directory argument (wide); fixture paths are UTF-8.
std::wstring wide_path(const SandboxDir& dir) {
    return folderpilot::platform::to_utf16(dir.u8()).value;
}

} // namespace

TEST_CASE("CLI dry-run prints the report and changes nothing") {
    SandboxDir dir{"cli-dryrun"};
    REQUIRE(create_file(dir.path(), L"resume.pdf", "r"));
    REQUIRE(create_file(dir.path(), L"photo.jpg", "p"));

    const auto before = take_snapshot(dir.path());
    const auto run = run_folderpilot_cli({wide_path(dir)});
    REQUIRE(run.exit_code == 0);
    CHECK(run.output.find("FOLDERPILOT - DRY RUN") != std::string::npos);
    CHECK(run.output.find("resume.pdf -> Documents/resume.pdf") != std::string::npos);
    CHECK(run.output.find("NO FILES WERE MOVED.") != std::string::npos);

    const auto after = take_snapshot(dir.path());
    CHECK(snapshots_equal(before, after));
    // Missing category directories were NOT created by the dry run.
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"Documents"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"Images"));
}

TEST_CASE("CLI rejects --apply explicitly without touching anything") {
    SandboxDir dir{"cli-apply"};
    REQUIRE(create_file(dir.path(), L"resume.pdf", "r"));

    const auto before = take_snapshot(dir.path());
    const auto run = run_folderpilot_cli({wide_path(dir), L"--apply"});
    CHECK(run.exit_code != 0);
    CHECK(run.output.find("File movement is not available in this build. "
                          "No changes were made.") != std::string::npos);
    const auto after = take_snapshot(dir.path());
    CHECK(snapshots_equal(before, after));
}

TEST_CASE("CLI refuses a missing target with the safety exit code") {
    SandboxDir dir{"cli-missing"};
    const auto run = run_folderpilot_cli({wide_path(dir) + L"\\does-not-exist"});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("Target rejected") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir.path() / L"does-not-exist"));
}

TEST_CASE("CLI --help and --version succeed without a target") {
    const auto help = run_folderpilot_cli({L"--help"});
    CHECK(help.exit_code == 0);
    CHECK(help.output.find("Usage:") != std::string::npos);

    const auto version = run_folderpilot_cli({L"--version"});
    CHECK(version.exit_code == 0);
    CHECK(version.output.find("FolderPilot") != std::string::npos);
}

TEST_CASE("CLI usage errors return non-zero") {
    SandboxDir dir{"cli-usage"};
    const auto no_args = run_folderpilot_cli({});
    CHECK(no_args.exit_code != 0);
    CHECK(no_args.output.find("Usage:") != std::string::npos);

    const auto unknown = run_folderpilot_cli({L"--frobnicate"});
    CHECK(unknown.exit_code != 0);
}

TEST_CASE("CLI dry-run output is deterministic for an unchanged fixture") {
    SandboxDir dir{"cli-deterministic"};
    REQUIRE(create_file(dir.path(), L"resume.pdf", "r"));
    REQUIRE(create_file(dir.path(), L"notes.txt", "n"));

    const auto first = run_folderpilot_cli({wide_path(dir)});
    const auto second = run_folderpilot_cli({wide_path(dir)});
    REQUIRE(first.exit_code == 0);
    REQUIRE(second.exit_code == 0);
    CHECK(first.output == second.output);
}
