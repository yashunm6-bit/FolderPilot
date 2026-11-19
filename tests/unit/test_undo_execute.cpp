// Phase 7.3 tests: undo execution with append-only attempt segments
// (§6.2/§9; T-52..T-62b analogues). Fixtures are synthetic directories under
// the workspace sandbox; every forward apply is a fully journaled run.

#include <doctest.h>

#include "folderpilot/undo_execute.hpp"
#include "folderpilot/executor.hpp"
#include "folderpilot/journal.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "folderpilot/undo_plan.hpp"
#include "../phase3_fixture.hpp"
#include "native.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace folderpilot;

struct UndoExecFixture {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    std::string root_key;
    bool storage_ok = false;

    explicit UndoExecFixture(const std::string& name)
        : base(name + "-base"), organized(name + "-root") {
        root = platform::open_root(organized.u8());
        auto storage = open_storage_root(base.u8(), root.root);
        storage_ok = storage.ok;
        root_key = storage.root_key;
    }

    std::filesystem::path journal_path(unsigned long long seq) const {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "journal-%020llu.jsonl",
                      static_cast<unsigned long long>(seq));
        return std::filesystem::path(base.u8()) / "FolderPilot" / "journals" /
               root_key / buf;
    }
    std::filesystem::path newest_segment_path(unsigned long long seq) const {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "undo-%020llu-%020llu.jsonl",
                      static_cast<unsigned long long>(seq),
                      static_cast<unsigned long long>(1));
        return std::filesystem::path(base.u8()) / "FolderPilot" / "journals" /
               root_key / buf;
    }
};

// Plans and executes one journaled forward run over the fixture's files.
ExecutionResult journal_apply(UndoExecFixture& f) {
    const auto scan = scan_directory(f.root.root);
    const auto plan = build_plan(f.root.root, scan);
    if (!plan.valid()) {
        ExecutionResult failed;
        failed.aborted = true;
        return failed;
    }
    return execute_plan(plan, f.organized.u8(), f.base.u8(), false);
}

std::string read_leaf_bytes(const StorageRoot& storage, const std::string& name) {
    auto leaf = open_journal_leaf_read(storage, name);
    if (!leaf.ok || !leaf.handle.valid()) {
        return {};
    }
    std::string contents;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(leaf.handle.get(), buffer, sizeof(buffer), &read, nullptr) &&
           read > 0) {
        contents.append(buffer, static_cast<std::size_t>(read));
    }
    return contents;
}

} // namespace

TEST_CASE("T-52: undo execution restores the file at its original location") {
    UndoExecFixture f("ue-restore");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);
    REQUIRE(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    REQUIRE(plan.fold.candidates.size() == 1);
    REQUIRE(plan.fold.candidates[0].classification == UndoCandidateClass::Undoable);

    const auto result = execute_undo(plan, f.base.u8(), f.root.root, true);
    CHECK(result.segment_active);
    CHECK_FALSE(result.segment_stopped);
    CHECK(result.eligible_count == 1);
    CHECK(result.restored_count == 1);
    CHECK(result.all_restored());
    // The file is back at its original slot with identical content, and the
    // destination slot is empty again.
    CHECK(std::filesystem::exists(f.organized.path() / L"report.pdf"));
    CHECK_FALSE(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));

    // The segment file holds UNDO-START, UNDO-INTENT, UNDO-RESULT, UNDO-END
    // with the ratified chaining and digest binding (§5/§6.2).
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);
    const std::string seg_bytes = read_leaf_bytes(storage, result.records.empty()
        ? std::string()
        : [&] {
              // Recover the segment name by listing: exactly one undo file.
              return std::string("undo-00000000000000000001-00000000000000000001.jsonl");
          }());
    REQUIRE_FALSE(seg_bytes.empty());
    const std::vector<unsigned char> image(seg_bytes.begin(), seg_bytes.end());
    const auto parsed = parse_journal_file(image, f.root_key);
    INFO("segment parse: valid=" << parsed.valid << " detail=" << parsed.failure.detail);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.records.size() == 4);
    const auto* start = std::get_if<JournalUndoStart>(&parsed.records[0]);
    REQUIRE(start != nullptr);
    CHECK(start->root_run_seq == 1);
    CHECK(start->undo_attempt_seq == 1);
    CHECK(start->attempt_candidate_count == 1);
    CHECK(start->journal_id.size() == 64);
    const auto* intent = std::get_if<JournalUndoIntent>(&parsed.records[1]);
    REQUIRE(intent != nullptr);
    CHECK(intent->undo_of_move == 0);
    const auto* jr = std::get_if<JournalUndoResult>(&parsed.records[2]);
    REQUIRE(jr != nullptr);
    CHECK(jr->outcome == JournalOutcome::Restored);
    CHECK(std::holds_alternative<JournalUndoEnd>(parsed.records[3]));

    // §5: the segment's fwd_journal_digest binds it to the forward journal's
    // byte image.
    const std::string fwd_bytes = read_leaf_bytes(storage, "journal-00000000000000000001.jsonl");
    const std::vector<unsigned char> fwd_image(fwd_bytes.begin(), fwd_bytes.end());
    const auto binding = verify_segment_digest(parsed, fwd_image);
    CHECK(binding.ok);
}

TEST_CASE("T-58: double undo reports nothing eligible and creates no segment") {
    UndoExecFixture f("ue-double");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    const auto first = execute_undo(plan, f.base.u8(), f.root.root, true);
    REQUIRE(first.restored_count == 1);

    // The second undo: the fold removes the restored move - nothing eligible,
    // no segment created (§9.1/§9.5).
    const auto plan2 = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan2.ok);
    CHECK(plan2.fold.candidates.empty());
    const auto second = execute_undo(plan2, f.base.u8(), f.root.root, true);
    CHECK(second.ok_shape);
    CHECK_FALSE(second.segment_active);
    CHECK(second.restored_count == 0);
}

TEST_CASE("T-55/T-53: unauthorized undo refuses; reconciliation refusals are reported unjournaled") {
    UndoExecFixture f("ue-refuse");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);
    const auto dest = f.organized.path() / L"Documents" / L"report.pdf";
    REQUIRE(std::filesystem::exists(dest));

    // Without the explicit authorization: refused before any change.
    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    const auto unauthorized = execute_undo(plan, f.base.u8(), f.root.root, false);
    CHECK(unauthorized.aborted);
    CHECK_FALSE(unauthorized.segment_active);

    SUBCASE("destination deleted: classified missing - nothing eligible, no segment (§9.1/T-58)") {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        REQUIRE_FALSE(ec);
        const auto plan2 = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan2.ok);
        REQUIRE(plan2.fold.candidates.size() == 1);
        CHECK(plan2.fold.candidates[0].classification == UndoCandidateClass::Missing);
        const auto result = execute_undo(plan2, f.base.u8(), f.root.root, true);
        // The candidate was refused pre-INTENT (§6.2): with nothing eligible,
        // no segment is created and the run reports nothing-to-undo.
        CHECK_FALSE(result.segment_active);
        CHECK(result.refused_count == 1);
        CHECK(result.eligible_count == 0);
        REQUIRE(result.records.size() == 1);
        CHECK_FALSE(result.records[0].journaled);
    }
    SUBCASE("destination replaced: classified replaced - nothing eligible, no segment") {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(fixture::create_file(f.organized.path() / L"Documents",
                                     L"report.pdf", "a replacement"));
        const auto plan2 = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan2.ok);
        const auto result = execute_undo(plan2, f.base.u8(), f.root.root, true);
        CHECK(result.refused_count == 1);
        CHECK(result.eligible_count == 0);
        CHECK_FALSE(result.segment_active);
        REQUIRE(result.records.size() == 1);
        CHECK_FALSE(result.records[0].journaled);
    }
    SUBCASE("source slot refilled after planning: rename-time Conflict journaled (T-53)") {
        // The plan is loaded while the source slot is still free (the
        // reconciliation passes); the slot is refilled afterwards - the
        // reverse rename's ReplaceIfExists=FALSE then enforces the Conflict
        // at rename time (R16), journaled as a failed UNDO-RESULT.
        const auto plan2 = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan2.ok);
        REQUIRE(plan2.fold.candidates.size() == 1);
        CHECK(plan2.fold.candidates[0].classification == UndoCandidateClass::Undoable);
        REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "new file"));
        const auto result = execute_undo(plan2, f.base.u8(), f.root.root, true);
        REQUIRE(result.eligible_count == 1);
        CHECK(result.conflict_count == 1);
        REQUIRE(result.records.size() == 1);
        CHECK(result.records[0].outcome == "Conflict");
        CHECK(result.records[0].journaled);
        // R16: the destination file is untouched (no overwrite happened).
        CHECK(std::filesystem::exists(dest));
    }
}

TEST_CASE("T-48 analogue: a blocked newest chain refuses undo execution") {
    UndoExecFixture f("ue-blocked");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);

    // Corrupt bytes inside the newest journal: the chain is blocked.
    std::fstream corrupter(f.journal_path(1),
                           std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(corrupter.is_open());
    corrupter.seekp(20);
    corrupter << "XXXX";
    corrupter.close();

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::Blocked);
    const auto result = execute_undo(plan, f.base.u8(), f.root.root, true);
    CHECK(result.aborted);
    CHECK(result.abort_exit_code == 2);
    CHECK_FALSE(result.segment_active);
}

TEST_CASE("lock contention refuses undo execution before the segment") {
    UndoExecFixture f("ue-locked");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);

    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);
    auto lock = acquire_mutation_lock(storage);
    REQUIRE(lock.ok);

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    const auto result = execute_undo(plan, f.base.u8(), f.root.root, true);
    CHECK(result.aborted);
    CHECK(result.abort_exit_code == 2);
    CHECK_FALSE(result.segment_active);
}
