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

TEST_CASE("CLI --apply performs real movement inside the sandbox") {
    // Phase 5 supersedes the Phase 4 rejection test: --apply is now the
    // explicit authorization for movement (this fixture is synthetic and
    // sandbox-confined). On OS builds where the approved RootDirectory-
    // relative rename is blocked (ERROR_INVALID_PARAMETER), the CLI must
    // fail safely: exit 3, sources untouched, nothing overwritten.
    SandboxDir dir{"cli-apply"};
    REQUIRE(create_file(dir.path(), L"resume.pdf", "resume-bytes"));
    REQUIRE(create_file(dir.path(), L"photo.jpg", "photo-bytes"));

    const auto run = run_folderpilot_cli({wide_path(dir), L"--apply"});
    CHECK(run.output.find("EXECUTION REPORT") != std::string::npos);
    CHECK(run.output.find("Batch execution is not transactional") != std::string::npos);

    if (root_relative_rename_supported()) {
        CHECK(run.exit_code == 0);
        CHECK(run.output.find("resume.pdf -> Documents/resume.pdf: Moved") != std::string::npos);
        CHECK(run.output.find("photo.jpg -> Images/photo.jpg: Moved") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(dir.path() / L"resume.pdf"));
        CHECK_FALSE(std::filesystem::exists(dir.path() / L"photo.jpg"));
        CHECK(std::filesystem::exists(dir.path() / L"Documents" / L"resume.pdf"));
        CHECK(std::filesystem::exists(dir.path() / L"Images" / L"photo.jpg"));
    } else {
        // Blocked-OS path: every move fails safely, sources untouched.
        CHECK(run.exit_code == 3);
        CHECK(run.output.find(": Failed") != std::string::npos);
        CHECK(run.output.find("Execution completed with partial failures.") != std::string::npos);
        CHECK(std::filesystem::exists(dir.path() / L"resume.pdf"));
        CHECK(std::filesystem::exists(dir.path() / L"photo.jpg"));
        CHECK_FALSE(std::filesystem::exists(dir.path() / L"Documents" / L"resume.pdf"));
    }
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
