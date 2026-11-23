// Phase 7.2 tests: read-only undo planning (§9 fold + §9.3 reconciliation).
// Fixtures are synthetic directories under the workspace sandbox; every
// forward apply in these tests is a fully journaled Phase 7.2 run.

#include <doctest.h>

#include "folderpilot/undo_plan.hpp"
#include "folderpilot/executor.hpp"
#include "folderpilot/journal_session.hpp"
#include "folderpilot/journal.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/storage_adapter.hpp"
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

// Synthetic organized root + injected storage base, with the storage chain
// bootstrapped eagerly (planted entries land inside <root-key>).
struct UndoFixture {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    std::string root_key;
    bool storage_ok = false;

    explicit UndoFixture(const std::string& name)
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
};

// Plans and executes one journaled forward run over the fixture's files.
ExecutionResult journal_apply(UndoFixture& f) {
    const auto scan = scan_directory(f.root.root);
    const auto plan = build_plan(f.root.root, scan);
    if (!plan.valid()) {
        ExecutionResult failed;
        failed.aborted = true;
        return failed;
    }
    return execute_plan(plan, f.organized.u8(), f.base.u8(), false);
}

// Reads a storage leaf back through the ratified read open.
std::string read_storage_leaf(const StorageRoot& storage, const std::string& name) {
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

TEST_CASE("W4: a fresh storage root reports no forward run") {
    UndoFixture f("up-fresh");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::AbandonedForwardInit);
    CHECK(plan.fold.candidates.empty());
}

TEST_CASE("W4: a journaled apply produces an Undoable candidate") {
    UndoFixture f("up-undoable");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));

    const auto applied = journal_apply(f);
    REQUIRE_FALSE(applied.aborted);
    REQUIRE(applied.journal_active);
    REQUIRE(applied.moved_count == 1);

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::Closed);
    REQUIRE(plan.fold.candidates.size() == 1);
    const auto& c = plan.fold.candidates[0];
    CHECK(c.classification == UndoCandidateClass::Undoable);
    CHECK(c.move_id == 0);
    CHECK(c.source_name == "report.pdf");
    CHECK(c.dest_name == "report.pdf");
    CHECK(c.file_id_hex.size() == 64);
    CHECK(c.file_ctime_filetime != 0);
    CHECK(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));
}

TEST_CASE("W4: reconciliation classifies already-reversed, missing, replaced and conflicting states") {
    UndoFixture f("up-reconcile");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);
    const auto dest = f.organized.path() / L"Documents" / L"report.pdf";
    const auto src = f.organized.path() / L"report.pdf";
    REQUIRE(std::filesystem::exists(dest));

    SUBCASE("moved back by hand: already reversed") {
        std::error_code ec;
        std::filesystem::rename(dest, src, ec);
        REQUIRE_FALSE(ec);
        const auto plan = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan.ok);
        REQUIRE(plan.fold.candidates.size() == 1);
        CHECK(plan.fold.candidates[0].classification == UndoCandidateClass::AlreadyReversed);
    }
    SUBCASE("destination deleted: missing") {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        REQUIRE_FALSE(ec);
        const auto plan = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan.ok);
        REQUIRE(plan.fold.candidates.size() == 1);
        CHECK(plan.fold.candidates[0].classification == UndoCandidateClass::Missing);
    }
    SUBCASE("destination replaced by a different file: replaced") {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(fixture::create_file(f.organized.path() / L"Documents",
                                     L"report.pdf", "a replacement"));
        const auto plan = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan.ok);
        REQUIRE(plan.fold.candidates.size() == 1);
        CHECK(plan.fold.candidates[0].classification == UndoCandidateClass::Replaced);
    }
    SUBCASE("source slot refilled while the destination exists: conflict") {
        REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "new file"));
        const auto plan = load_undo_plan(f.base.u8(), f.root.root);
        REQUIRE(plan.ok);
        REQUIRE(plan.fold.candidates.size() == 1);
        CHECK(plan.fold.candidates[0].classification == UndoCandidateClass::Conflict);
    }
}

TEST_CASE("W4: a torn RUN-END terminal is disclosed and the run stays undoable") {
    UndoFixture f("up-torn");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));

    std::string planned_file_id;
    unsigned long long planned_ctime = 0;

    // Plan the move to capture the file's REAL fingerprint (identity digest
    // + creation time) for the journal records.
    {
        const auto scan = scan_directory(f.root.root);
        const auto plan = build_plan(f.root.root, scan);
        REQUIRE(plan.valid());
        REQUIRE(plan.planned_count() == 1);
        REQUIRE(storage_leaf_id(plan.moves()[0].identity, planned_file_id));
        planned_ctime = plan.moves()[0].creation_time_filetime;
    }

    // The session records INTENT + RESULT (both anchored) with the real
    // fingerprint, the file is actually moved, but NOT the RUN-END; a torn
    // RUN-END terminal is then appended by hand - the ratified terminal rules
    // (§7/R2) resolve the run and disclose the torn END - the folded
    // candidates remain undoable.
    std::string result_integrity;
    {
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false);
        REQUIRE(opened.ok);
        auto storage = open_storage_root(f.base.u8(), f.root.root);
        REQUIRE(storage.ok);
        JournalIntent intent;
        intent.move_id = 0;
        intent.cat_id = std::string(64, 'c');
        intent.file_id = planned_file_id;
        intent.file_ctime = planned_ctime;
        intent.source_name = "report.pdf";
        intent.dest_name = "report.pdf";
        REQUIRE(opened.session.record_intent(intent).ok);
        JournalResult jr;
        jr.move_id = 0;
        jr.cat_id = intent.cat_id;
        jr.file_id = intent.file_id;
        jr.file_ctime = intent.file_ctime;
        jr.source_name = "report.pdf";
        jr.dest_name = "report.pdf";
        jr.outcome = JournalOutcome::Moved;
        const auto recorded = opened.session.record_result(jr);
        INFO("record_result: ok=" << recorded.ok << " failure="
             << static_cast<int>(recorded.failure) << " error=" << recorded.error);
        REQUIRE(recorded.ok);

        // Capture the RESULT's integrity (the torn RUN-END's prev) while the
        // session holds the journal; the torn append happens after the
        // session releases its share-read handle.
        const std::string journal_bytes =
            read_storage_leaf(storage, opened.session.journal_name());
        const std::vector<unsigned char> image(journal_bytes.begin(), journal_bytes.end());
        const auto parsed = parse_journal_file(image, f.root_key);
        REQUIRE(parsed.valid);
        REQUIRE(parsed.records.size() == 3);
        result_integrity = journal_hex(parsed.record_digests[2]);
        // The session (and its handles) are released at this scope's end.
    }

    // The actual move (the journal already recorded it as Moved).
    std::filesystem::create_directories(f.organized.path() / L"Documents");
    std::error_code move_ec;
    std::filesystem::rename(f.organized.path() / L"report.pdf",
                            f.organized.path() / L"Documents" / L"report.pdf",
                            move_ec);
    REQUIRE_FALSE(move_ec);

    // Torn RUN-END: serialized with the RESULT's integrity as prev; only a
    // prefix is written (the terminal is interrupted mid-write).
    JournalRunEnd end;
    end.run = "run-1";
    end.root_run_seq = 1;
    end.planned_count = 1;
    end.moved_count = 1;
    auto torn = serialize_journal_record(JournalRecord{end}, result_integrity);
    INFO("torn serialize: ok=" << torn.ok << " error=" << torn.error);
    REQUIRE(torn.ok);
    std::ofstream appender(f.journal_path(1), std::ios::binary | std::ios::app);
    REQUIRE(appender.is_open());
    appender.write(torn.canonical.data(),
                   static_cast<std::streamsize>(torn.canonical.size() - 10));
    appender.close();
    // The journal now holds RUN-START, INTENT, RESULT and a torn RUN-END.

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    INFO("undo plan: ok=" << plan.ok << " err=" << plan.error
         << " state=" << static_cast<int>(plan.fold.state)
         << " reason=" << static_cast<int>(plan.fold.reason)
         << " cands=" << plan.fold.candidates.size()
         << (plan.fold.candidates.size() == 1
                 ? " class=" + std::to_string(static_cast<int>(
                       plan.fold.candidates[0].classification)) +
                       " detail=" + plan.fold.candidates[0].detail
                 : ""));
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::CompleteUnterminated);
    CHECK(plan.fold.torn_terminal_end);
    REQUIRE(plan.fold.candidates.size() == 1);
    CHECK(plan.fold.candidates[0].classification == UndoCandidateClass::Undoable);
}

TEST_CASE("W4: bytes written after the terminal RUN-END block undo planning") {
    UndoFixture f("up-postterminal");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);
    REQUIRE(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));

    // Bytes written AFTER the terminal RUN-END are a protocol violation
    // (T-64: records after RUN-END) - fail closed.
    std::ofstream garbage(f.journal_path(1), std::ios::binary | std::ios::app);
    REQUIRE(garbage.is_open());
    garbage << "{\"v\":1,\"run\":\"garbage";
    garbage.close();

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::Blocked);
    CHECK(plan.fold.candidates.empty());
}

TEST_CASE("W4: mid-file corruption blocks undo planning") {
    UndoFixture f("up-corrupt");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    const auto applied = journal_apply(f);
    REQUIRE(applied.moved_count == 1);

    // Corrupt bytes INSIDE the RUN-START record: the journal no longer
    // parses - undo planning is blocked (fail closed).
    std::fstream corrupter(f.journal_path(1),
                           std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(corrupter.is_open());
    corrupter.seekp(20);
    corrupter << "XXXX";
    corrupter.close();

    const auto plan = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.fold.state == ForwardChainState::Blocked);
    CHECK(plan.fold.reason == ForwardBlockReason::ParseInvalid);
    CHECK(plan.fold.candidates.empty());
}
