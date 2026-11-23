// Phase 8 tests: retry planning and execution (docs/PHASE8-DESIGN.md §1.4).

#include <doctest.h>

#include "folderpilot/executor.hpp"
#include "folderpilot/journal.hpp"
#include "folderpilot/retry_plan.hpp"
#include "folderpilot/journal_session.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/storage_adapter.hpp"

#include <algorithm>
#include <fstream>
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
#include <string>

namespace {

using namespace folderpilot;

struct RetryFixture {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    std::string root_key;
    bool storage_ok = false;

    explicit RetryFixture(const std::string& name)
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

// Simulates a forward run that recorded `outcome` for report.pdf without
// moving it (the executor records Locked/AccessDenied as journaled failures).
JournalSession open_session_with_locked_outcome(RetryFixture& f,
                                                JournalOutcome outcome) {
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false);
    REQUIRE(opened.ok);
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);

    // The planned fingerprint must be the file's REAL one (the retry
    // preflight compares against it).
    const auto scan = scan_directory(f.root.root);
    const auto plan = build_plan(f.root.root, scan);
    REQUIRE(plan.valid());
    REQUIRE(plan.planned_count() == 1);
    std::string file_id;
    REQUIRE(storage_leaf_id(plan.moves()[0].identity, file_id));
    const auto ctime = plan.moves()[0].creation_time_filetime;

    JournalIntent intent;
    intent.move_id = 0;
    intent.cat_id = std::string(64, 'c');
    intent.file_id = file_id;
    intent.file_ctime = ctime;
    intent.file_size = 7;
    intent.source_name = "report.pdf";
    intent.dest_name = "report.pdf";
    REQUIRE(opened.session.record_intent(intent).ok);
    JournalResult jr;
    jr.move_id = 0;
    jr.cat_id = intent.cat_id;
    jr.file_id = file_id;
    jr.file_ctime = ctime;
    jr.file_size = 7;
    jr.source_name = "report.pdf";
    jr.dest_name = "report.pdf";
    jr.outcome = outcome;
    REQUIRE(opened.session.record_result(jr).ok);
    JournalRunEnd end;
    end.planned_count = 1;
    end.failed_count = 1;
    REQUIRE(opened.session.end_run(end).ok);
    // The file stays at the source (the outcome says the rename failed).
    REQUIRE(std::filesystem::exists(f.organized.path() / L"report.pdf"));
    return std::move(opened.session);
}

} // namespace

TEST_CASE("P8: a Locked outcome is retryable after a fresh preflight") {
    RetryFixture f{"rt-locked"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::Locked);
        // The session holds the mutation lock; it MUST be released before the
        // retry read acquires its own lock. open_session_with_locked_outcome
        // already wrote the terminal RUN-END; a second end_run is an invariant
        // violation, not a check.
    }

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    INFO("retry: ok=" << plan.ok << " blocked=" << plan.blocked
         << " detail=" << plan.blocked_detail);
    REQUIRE(plan.ok);
    REQUIRE(plan.candidates.size() == 1);
    CHECK(plan.candidates[0].recorded_outcome == JournalOutcome::Locked);
    CHECK(plan.candidates[0].preflight_ok);
    CHECK(plan.not_retryable == 0);
}

TEST_CASE("P8: an AccessDenied outcome is retryable after a fresh preflight") {
    RetryFixture f{"rt-denied"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::AccessDenied);
    }

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    REQUIRE(plan.candidates.size() == 1);
    CHECK(plan.candidates[0].preflight_ok);
}

TEST_CASE("P8: Moved and permanent outcomes are never retryable") {
    RetryFixture f{"rt-permanent"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    {
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 2, false);
        REQUIRE(opened.ok);
        JournalIntent intent;
        intent.move_id = 0;
        intent.cat_id = std::string(64, 'c');
        intent.file_id = std::string(64, 'f');
        intent.source_name = "report.pdf";
        intent.dest_name = "report.pdf";
        REQUIRE(opened.session.record_intent(intent).ok);
        JournalResult moved;
        moved.move_id = 0;
        moved.cat_id = intent.cat_id;
        moved.file_id = intent.file_id;
        moved.source_name = intent.source_name;
        moved.dest_name = intent.dest_name;
        moved.dest_cat = intent.dest_cat;
        moved.outcome = JournalOutcome::Moved;
        REQUIRE(opened.session.record_result(moved).ok);
        JournalIntent intent2;
        intent2.move_id = 1;
        intent2.cat_id = std::string(64, 'c');
        intent2.file_id = std::string(64, 'e');
        intent2.source_name = "other.txt";
        intent2.dest_name = "other.txt";
        intent2.dest_cat = static_cast<Category>(1);
        REQUIRE(opened.session.record_intent(intent2).ok);
        JournalResult conflict;
        conflict.move_id = 1;
        conflict.cat_id = intent2.cat_id;
        conflict.file_id = intent2.file_id;
        conflict.source_name = intent2.source_name;
        conflict.dest_name = intent2.dest_name;
        conflict.dest_cat = intent2.dest_cat;
        conflict.outcome = JournalOutcome::Conflict;
        REQUIRE(opened.session.record_result(conflict).ok);
        JournalRunEnd end;
        end.planned_count = 2;
        end.moved_count = 1;
        end.conflict_count = 1;
        REQUIRE(opened.session.end_run(end).ok);
    }

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.candidates.empty());
    CHECK(plan.not_retryable == 2);
}

TEST_CASE("P8: a vanished source fails the retry preflight (not retried)") {
    RetryFixture f{"rt-vanished"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::Locked);
    }
    // The source vanishes after the journal records the Locked outcome.
    std::error_code ec;
    std::filesystem::remove(f.organized.path() / L"report.pdf", ec);
    REQUIRE_FALSE(ec);

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    REQUIRE(plan.candidates.size() == 1);
    CHECK_FALSE(plan.candidates[0].preflight_ok);
}

TEST_CASE("P8: a fingerprint mismatch fails the retry preflight (file replaced)") {
    RetryFixture f{"rt-fingerprint"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::Locked);
    }
    // The file is replaced by a different one (new identity + ctime).
    std::error_code ec;
    std::filesystem::remove(f.organized.path() / L"report.pdf", ec);
    REQUIRE_FALSE(ec);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "replaced"));

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    REQUIRE(plan.candidates.size() == 1);
    CHECK_FALSE(plan.candidates[0].preflight_ok);
}

TEST_CASE("P8: a blocked newest journal refuses the retry (exit 2 shape)") {
    RetryFixture f{"rt-blocked"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "content"));
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::Locked);
        // The session is released at the scope end (its share-read journal
        // handle would conflict with the read-write corruption open).
    }
    // Corrupt bytes inside the newest journal.
    std::fstream corrupter(f.journal_path(1),
                           std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(corrupter.is_open());
    corrupter.seekp(20);
    corrupter << "XXXX";
    corrupter.close();

    const auto plan = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(plan.ok);
    CHECK(plan.blocked);
}

TEST_CASE("P8: retry apply runs a fresh journaled run and never rewrites the original journal") {
    RetryFixture f{"rt-fresh"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    {
        auto session = open_session_with_locked_outcome(f, JournalOutcome::Locked);
        // The session (and its journal handle) releases at the scope end.
    }
    const auto original = f.journal_path(1);
    REQUIRE(std::filesystem::exists(original));
    std::string before;
    {
        std::ifstream in(original, std::ios::binary);
        REQUIRE(in.is_open());
        before.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
    }
    REQUIRE_FALSE(before.empty());

    const auto retry = load_retry_plan(f.base.u8(), f.root.root);
    REQUIRE(retry.ok);
    REQUIRE(retry.candidates.size() == 1);
    REQUIRE(retry.candidates[0].preflight_ok);

    // The retry apply mirrors the CLI dispatch: a fresh scan filtered to
    // the preflighted candidates (name AND identity digest AND creation
    // time), planned and executed as a fresh fully-journaled run.
    const auto scan = scan_directory(f.root.root);
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    ScanResult filtered;
    filtered.enumeration = EnumerationStatus::Ok;
    filtered.root = scan.root;
    for (auto& file : scan.files) {
        std::string live_digest;
        if (file.name_utf8 == retry.candidates[0].source_name &&
            storage_leaf_id(file.identity, live_digest) &&
            live_digest == retry.candidates[0].file_id_hex &&
            file.creation_time_filetime == retry.candidates[0].file_ctime_filetime) {
            filtered.files.push_back(std::move(file));
        }
    }
    REQUIRE(filtered.files.size() == 1);
    const auto fresh_plan = build_plan(f.root.root, filtered);
    INFO("fresh plan: valid=" << fresh_plan.valid() << " err=" << fresh_plan.error());
    REQUIRE(fresh_plan.valid());
    REQUIRE(fresh_plan.planned_count() == 1);

    const auto result = execute_plan(fresh_plan, f.organized.u8(), f.base.u8(), true);
    REQUIRE(result.journal_active);
    REQUIRE(result.moved_count == 1);
    CHECK(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));

    // The retry is a NEW journal with a NEW sequence identity; the original
    // journal is never rewritten.
    const auto fresh = f.journal_path(2);
    REQUIRE(std::filesystem::exists(fresh));
    CHECK(fresh != original);
    std::string after;
    {
        std::ifstream in(original, std::ios::binary);
        REQUIRE(in.is_open());
        after.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    CHECK(after == before);

    // The fresh journal carries the retry run's Moved outcome for the file.
    std::string fresh_bytes;
    {
        std::ifstream in(fresh, std::ios::binary);
        REQUIRE(in.is_open());
        fresh_bytes.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }
    const std::vector<unsigned char> fresh_image(fresh_bytes.begin(),
                                                 fresh_bytes.end());
    const auto parsed = parse_journal_file(fresh_image, f.root_key);
    REQUIRE(parsed.valid);
    bool saw_moved = false;
    for (const auto& rec : parsed.records) {
        if (const auto* r = std::get_if<JournalResult>(&rec)) {
            if (r->outcome == JournalOutcome::Moved && r->source_name == "report.pdf") {
                saw_moved = true;
            }
        }
    }
    CHECK(saw_moved);
}
