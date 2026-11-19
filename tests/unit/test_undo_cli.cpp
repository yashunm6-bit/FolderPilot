// Phase 7.4 tests: the undo CLI surface (T-01..T-07 analogues, §2 grammar).
// Every scenario runs the real executable against synthetic fixtures.

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../phase3_fixture.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/executor.hpp"
#include "folderpilot/undo_plan.hpp"
#include "folderpilot/undo_execute.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"

using namespace folderpilot;
using namespace fixture;

namespace {

struct CliUndoFixture {
    SandboxDir base;
    SandboxDir organized;
    platform::RootOpenResult root;

    explicit CliUndoFixture(const std::string& name)
        : base{name + "-base"}, organized{name + "-root"} {
        root = platform::open_root(organized.u8());
    }
};

std::wstring wide_path(const std::filesystem::path& p) {
    auto preferred = p;
    preferred.make_preferred();
    return preferred.wstring();
}

} // namespace

TEST_CASE("T-01: undo preview on a fresh root reports nothing to undo (exit 0)") {
    CliUndoFixture f{"ucli-fresh"};
    REQUIRE(f.root.ok);
    const auto run = run_folderpilot_cli({wide_path(f.organized.path()), L"--undo"});
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("UNDO PREVIEW") != std::string::npos);
    CHECK(run.output.find("nothing to undo") != std::string::npos);
}

TEST_CASE("T-02: apply then undo execute restores the file (exit 0)") {
    CliUndoFixture f{"ucli-execute"};
    REQUIRE(f.root.ok);
    REQUIRE(create_file(f.organized.path(), L"report.pdf", "content"));

    const auto apply = run_folderpilot_cli({wide_path(f.organized.path()), L"--apply"});
    REQUIRE(apply.exit_code == 0);
    const auto dest = f.organized.path() / L"Documents" / L"report.pdf";
    REQUIRE(std::filesystem::exists(dest));

    const auto preview = run_folderpilot_cli({wide_path(f.organized.path()), L"--undo"});
    REQUIRE(preview.exit_code == 0);
    CHECK(preview.output.find("undoable") != std::string::npos);

    const auto run = run_folderpilot_cli({wide_path(f.organized.path()),
                                          L"--undo", L"--apply"});
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("UNDO EXECUTION") != std::string::npos);
    CHECK(run.output.find("restored") != std::string::npos);
    CHECK(std::filesystem::exists(f.organized.path() / L"report.pdf"));
    CHECK_FALSE(std::filesystem::exists(dest));
}

TEST_CASE("T-03: undo execute without a forward run refuses (exit 2)") {
    CliUndoFixture f{"ucli-empty"};
    REQUIRE(f.root.ok);
    const auto run = run_folderpilot_cli({wide_path(f.organized.path()),
                                          L"--undo", L"--apply"});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("nothing to undo") != std::string::npos);
}

TEST_CASE("T-04: the acknowledgement option without --apply is a usage error (exit 1)") {
    CliUndoFixture f{"ucli-ack"};
    REQUIRE(f.root.ok);
    const auto run = run_folderpilot_cli({wide_path(f.organized.path()),
                                          L"--acknowledge-undo-window"});
    CHECK(run.exit_code == 1);
}

TEST_CASE("T-05: the acknowledgement option with --undo is a usage error (exit 1)") {
    CliUndoFixture f{"ucli-ack-undo"};
    REQUIRE(f.root.ok);
    const auto run = run_folderpilot_cli({wide_path(f.organized.path()),
                                          L"--undo", L"--acknowledge-undo-window"});
    CHECK(run.exit_code == 1);
}

TEST_CASE("T-06: a second forward apply without the acknowledgement is refused (exit 2)") {
    CliUndoFixture f{"ucli-window"};
    REQUIRE(f.root.ok);
    REQUIRE(create_file(f.organized.path(), L"report.pdf", "first"));
    const auto first = run_folderpilot_cli({wide_path(f.organized.path()), L"--apply"});
    REQUIRE(first.exit_code == 0);
    REQUIRE(create_file(f.organized.path(), L"report.pdf", "second"));

    // A live undo window exists (§9.5): the second forward apply is refused,
    // flag-immune (§2: the ack option is forward-only).
    const auto refused = run_folderpilot_cli({wide_path(f.organized.path()), L"--apply"});
    CHECK(refused.exit_code == 2);
    CHECK(refused.output.find("journal session refused") != std::string::npos);
    // The refusal mutates nothing: the new file is still in the root.
    CHECK(std::filesystem::exists(f.organized.path() / L"report.pdf"));
}

TEST_CASE("T-07: a second forward apply with the acknowledgement proceeds (exit 0)") {
    CliUndoFixture f{"ucli-ack-ok"};
    REQUIRE(f.root.ok);
    REQUIRE(create_file(f.organized.path(), L"report.pdf", "first"));
    const auto first = run_folderpilot_cli({wide_path(f.organized.path()), L"--apply"});
    REQUIRE(first.exit_code == 0);
    REQUIRE(create_file(f.organized.path(), L"report.pdf", "second"));

    const auto second = run_folderpilot_cli(
        {wide_path(f.organized.path()), L"--apply", L"--acknowledge-undo-window"});
    CHECK(second.exit_code == 0);
    CHECK(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));
}
