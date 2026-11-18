// Phase 7.2 tests: the write-ahead journal session (docs/PHASE7-DESIGN.md §20).
// Fixtures are synthetic directories under the workspace sandbox (never
// %TEMP%, never user data). Smart App Control is permanently off by owner
// decision, so these cases execute locally as well as on hosted CI.

#include <doctest.h>

#include "folderpilot/journal_session.hpp"
#include "folderpilot/journal.hpp"
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
#include <vector>

namespace {

using namespace folderpilot;

// A synthetic organized root plus an injected storage base; the journal
// session operates entirely inside the sandbox. The storage chain is
// bootstrapped eagerly so planted entries land inside <root-key>.
struct SessionFixture {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    std::string root_key;
    bool storage_ok = false;

    explicit SessionFixture(const std::string& name)
        : base(name + "-base"), organized(name + "-root") {
        root = platform::open_root(organized.u8());
        auto storage = open_storage_root(base.u8(), root.root);
        storage_ok = storage.ok;
        root_key = storage.root_key;
    }

    std::filesystem::path root_key_dir() const {
        return std::filesystem::path(base.u8()) / "FolderPilot" / "journals" /
               root_key;
    }
    std::filesystem::path leaf_path(const std::string& name) const {
        return root_key_dir() / name;
    }
};

// Plants an empty file with the given name inside the root-key directory.
void plant_entry(const SessionFixture& f, const std::string& name) {
    std::filesystem::create_directories(f.root_key_dir());
    std::ofstream creator(f.leaf_path(name), std::ios::binary);
    REQUIRE(creator.is_open());
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

TEST_CASE("T-84: sequence allocation starts at 1 and advances by name") {
    SessionFixture f("js-alloc");
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);

    {
        auto first = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
        REQUIRE(first.ok);
        CHECK(first.session.root_run_seq() == 1);
        CHECK(first.session.journal_name() == "journal-00000000000000000001.jsonl");
        CHECK(first.session.anchor_name() == "anchor-00000000000000000001.jsonl");
        CHECK(first.session.run() == "run-1");
        CHECK(first.session.journal_id().size() == 64);
        CHECK(first.session.anchor_id().size() == 64);
        CHECK(first.session.lock_id().size() == 64);
        CHECK(first.session.root_id().size() == 64);
        REQUIRE(first.session.end_run(JournalRunEnd{}).ok);
        // The session (and its mutation lock) is released here before the
        // next run opens.
    }

    // The second run allocates the next sequence (the first run's files are
    // recognized storage entries).
    auto second = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    REQUIRE(second.ok);
    CHECK(second.session.root_run_seq() == 2);
    CHECK(second.session.journal_name() == "journal-00000000000000000002.jsonl");
    REQUIRE(second.session.end_run(JournalRunEnd{}).ok);
}

TEST_CASE("T-85/M1: a journal without its anchor is legal classification and the next sequence is allocated") {
    SessionFixture f("js-abandoned");
    REQUIRE(f.root.ok);
    plant_entry(f, "journal-00000000000000000001.jsonl");

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    REQUIRE(opened.ok);
    CHECK(opened.session.root_run_seq() == 2);
}

TEST_CASE("T-86: storage classification refuses malformed and unrecognized entries") {
    SessionFixture f("js-classify");
    REQUIRE(f.root.ok);

    SUBCASE("malformed recognized-looking names") {
        plant_entry(f, "journal-12a3.jsonl");
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
        CHECK_FALSE(opened.ok);
        CHECK(opened.failure == JournalSessionFailure::StorageClassification);
    }
    SUBCASE("unrecognized entries") {
        plant_entry(f, "stuff.txt");
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
        CHECK_FALSE(opened.ok);
        CHECK(opened.failure == JournalSessionFailure::StorageClassification);
    }
    SUBCASE("case-variant recognized-looking names") {
        plant_entry(f, "JOURNAL-00000000000000000009.jsonl");
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
        CHECK_FALSE(opened.ok);
        CHECK(opened.failure == JournalSessionFailure::StorageClassification);
    }
    SUBCASE("orphan anchor without its journal") {
        plant_entry(f, "anchor-00000000000000000001.jsonl");
        auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
        CHECK_FALSE(opened.ok);
        CHECK(opened.failure == JournalSessionFailure::StorageClassification);
    }
}

TEST_CASE("T-94: capacity exhaustion refuses before any file creation") {
    SessionFixture f("js-quota");
    REQUIRE(f.root.ok);
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);
    std::filesystem::create_directories(f.root_key_dir());
    // A sparse 129 MiB file exhausts the 128 MiB aggregate quota without
    // writing the bytes (resize_file reserves the size on NTFS).
    auto big = f.leaf_path("journal-00000000000000000001.jsonl");
    { std::ofstream creator(big, std::ios::binary); REQUIRE(creator.is_open()); }
    std::error_code ec;
    std::filesystem::resize_file(big, static_cast<std::uintmax_t>(128) * 1024 * 1024 + 1, ec);
    REQUIRE_FALSE(ec);

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    CHECK_FALSE(opened.ok);
    CHECK(opened.failure == JournalSessionFailure::CapacityExhausted);
}

TEST_CASE("T-87: RUN-START and the genesis anchor record are written and flushed") {
    SessionFixture f("js-runstart");
    REQUIRE(f.root.ok);

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 3, false, 0);
    REQUIRE(opened.ok);
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);

    // The journal holds exactly the RUN-START record with the ratified fields.
    const std::string journal_bytes = read_leaf_bytes(storage, opened.session.journal_name());
    const std::vector<unsigned char> image(journal_bytes.begin(), journal_bytes.end());
    const auto parsed = parse_journal_file(image, opened.session.root_id());
    REQUIRE(parsed.valid);
    REQUIRE(parsed.records.size() == 1);
    const auto* start = std::get_if<JournalRunStart>(&parsed.records[0]);
    REQUIRE(start != nullptr);
    CHECK(start->run == "run-1");
    CHECK(start->root_run_seq == 1);
    CHECK(start->planned_move_count == 3);
    CHECK(start->lock_id == opened.session.lock_id());
    CHECK(start->root_id == opened.session.root_id());
    CHECK(start->anchor_id == opened.session.anchor_id());
    CHECK_FALSE(start->has_ack);

    // The genesis anchor record covers seq 0 through the RUN-START's length
    // and its last_record_hash is the RUN-START's own integrity value.
    const std::string anchor_bytes = read_leaf_bytes(storage, opened.session.anchor_name());
    const std::vector<unsigned char> anchor_image(anchor_bytes.begin(), anchor_bytes.end());
    const auto anchor_parsed = parse_anchor_file(anchor_image);
    REQUIRE(anchor_parsed.valid);
    REQUIRE(anchor_parsed.records.size() == 1);
    CHECK(anchor_parsed.records[0].anchor_seq == 0);
    CHECK(anchor_parsed.records[0].covered_seq == 0);
    CHECK(anchor_parsed.records[0].byte_len == journal_bytes.size());
    CHECK(anchor_parsed.records[0].last_record_hash == journal_hex(parsed.record_digests[0]));
}

TEST_CASE("T-87/T-91: intent, result and run-end records chain with anchor coverage") {
    SessionFixture f("js-chain");
    REQUIRE(f.root.ok);

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    REQUIRE(opened.ok);
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);

    JournalIntent intent;
    intent.move_id = 0;
    intent.cat_id = std::string(64, 'c');
    intent.file_id = std::string(64, 'f');
    intent.file_ctime = 133000000000000000ULL;
    intent.file_size = 42;
    intent.file_mtime = 132999999900000000ULL;
    intent.source_name = "report.pdf";
    intent.dest_name = "report.pdf";
    auto recorded = opened.session.record_intent(intent);
    REQUIRE(recorded.ok);

    JournalResult result;
    result.move_id = 0;
    result.cat_id = intent.cat_id;
    result.file_id = intent.file_id;
    result.file_ctime = intent.file_ctime;
    result.file_size = intent.file_size;
    result.file_mtime = intent.file_mtime;
    result.source_name = "report.pdf";
    result.dest_name = "report.pdf";
    result.outcome = JournalOutcome::Moved;
    recorded = opened.session.record_result(result);
    REQUIRE(recorded.ok);

    recorded = opened.session.end_run(JournalRunEnd{});
    REQUIRE(recorded.ok);

    // The journal holds RUN-START, INTENT, RESULT, RUN-END with the ratified
    // prev chaining; the anchor covers exactly the three anchored records.
    const std::string journal_bytes = read_leaf_bytes(storage, opened.session.journal_name());
    const std::vector<unsigned char> image(journal_bytes.begin(), journal_bytes.end());
    const auto parsed = parse_journal_file(image, opened.session.root_id());
    INFO("journal parse: valid=" << parsed.valid << " kind="
         << static_cast<int>(parsed.failure.kind) << " off=" << parsed.failure.offset
         << " detail=" << parsed.failure.detail);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.records.size() == 4);
    CHECK(std::holds_alternative<JournalRunStart>(parsed.records[0]));
    CHECK(std::holds_alternative<JournalIntent>(parsed.records[1]));
    CHECK(std::holds_alternative<JournalResult>(parsed.records[2]));
    CHECK(std::holds_alternative<JournalRunEnd>(parsed.records[3]));
    CHECK(parsed.record_digests.size() == 4);

    const std::string anchor_bytes = read_leaf_bytes(storage, opened.session.anchor_name());
    const std::vector<unsigned char> anchor_image(anchor_bytes.begin(), anchor_bytes.end());
    const auto anchor_parsed = parse_anchor_file(anchor_image);
    REQUIRE(anchor_parsed.valid);
    REQUIRE(anchor_parsed.records.size() == 3); // RUN-END is an unanchored terminal
    CHECK(anchor_parsed.records[0].covered_seq == 0);
    CHECK(anchor_parsed.records[1].covered_seq == 1);
    CHECK(anchor_parsed.records[2].covered_seq == 2);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(anchor_parsed.records[i].last_record_hash == journal_hex(parsed.record_digests[i]));
    }
}

TEST_CASE("T-88: the session refuses records after the run has ended") {
    SessionFixture f("js-ended");
    REQUIRE(f.root.ok);
    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 0, false, 0);
    REQUIRE(opened.ok);
    REQUIRE(opened.session.end_run(JournalRunEnd{}).ok);
    auto again = opened.session.end_run(JournalRunEnd{});
    CHECK_FALSE(again.ok);
    CHECK(again.failure == JournalSessionFailure::InvalidState);
}

TEST_CASE("lock contention refuses the journal session") {
    SessionFixture f("js-locked");
    REQUIRE(f.root.ok);
    auto storage = open_storage_root(f.base.u8(), f.root.root);
    REQUIRE(storage.ok);
    auto lock = acquire_mutation_lock(storage);
    REQUIRE(lock.ok);

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    CHECK_FALSE(opened.ok);
    CHECK(opened.failure == JournalSessionFailure::LockHeld);
}

TEST_CASE("sequence overflow refuses new runs (T-94 analogue)") {
    SessionFixture f("js-overflow");
    REQUIRE(f.root.ok);
    plant_entry(f, "journal-18446744073709551615.jsonl");

    auto opened = JournalSession::open_for_run(f.base.u8(), f.root.root, 1, false, 0);
    CHECK_FALSE(opened.ok);
    CHECK(opened.failure == JournalSessionFailure::SequenceOverflow);
}
