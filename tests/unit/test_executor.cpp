// Phase 5 tests: safe executor (sandboxed integration).
// Every --apply scenario uses synthetic files inside build/phase5-test-sandbox/.

#include <doctest.h>

#include <algorithm>
#include <cstring>
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
#include "folderpilot/executor.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "text.hpp"

using namespace folderpilot;
using namespace fixture;

namespace {

// Validates, scans, and plans a fixture in one step (Phase 5 sandbox).
struct PlannedFixture {
    SandboxDir dir;
    PathGuardResult guard;
    ScanResult scan;
    OrganizationPlan plan;

    explicit PlannedFixture(const std::string& name)
        : dir{name, SandboxRoot::Phase5} {
        guard = validate_target(dir.u8());
        if (guard.ok) {
            scan = scan_directory(guard.root);
            plan = build_plan(guard.root, scan);
        }
    }
};

const MoveRecord* find_record(const ExecutionResult& result, const std::string& source) {
    for (const MoveRecord& r : result.records) {
        if (r.source_name == source) {
            return &r;
        }
    }
    return nullptr;
}

bool file_exists_with_content(const std::filesystem::path& p, const std::string& bytes) {
    // Delegates to the fixture helper (full share mode): this check must not
    // fail merely because the test itself holds a DELETE-capable handle on
    // the file being verified (the locked-source test does exactly that).
    return fixture::file_content_matches(p, bytes);
}

} // namespace


TEST_CASE("executor moves files into created and existing categories") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-basic"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"resume.pdf", "resume-content"));
    REQUIRE(fixture::create_file(fx.dir.path(), L"photo.jpg", "photo-content"));
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Code")); // pre-existing
    REQUIRE(fixture::create_file(fx.dir.path(), L"main.cpp", "code-content"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());
    REQUIRE(fx.plan.planned_count() == 3);

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    CHECK_FALSE(result.aborted);
    CHECK(result.planned_count == 3);
    CHECK(result.moved_count == 3);
    CHECK(result.conflict_count == 0);
    CHECK(result.skipped_count == 0);
    CHECK(result.failed_count == 0);
    // Missing categories were created; Code already existed.
    CHECK(result.created_directories.size() == 6);
    bool code_created = false;
    for (const std::string& name : result.created_directories) {
        if (name == "Code") {
            code_created = true;
        }
    }
    CHECK_FALSE(code_created);
    CHECK(std::filesystem::exists(fx.dir.path() / L"Documents" / L"resume.pdf"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"Images" / L"photo.jpg"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"Code" / L"main.cpp"));
    // Byte-identical content at the destinations.
    CHECK(file_exists_with_content(fx.dir.path() / L"Documents" / L"resume.pdf", "resume-content"));
    CHECK(file_exists_with_content(fx.dir.path() / L"Images" / L"photo.jpg", "photo-content"));
    CHECK(file_exists_with_content(fx.dir.path() / L"Code" / L"main.cpp", "code-content"));
    // Sources are gone.
    CHECK_FALSE(std::filesystem::exists(fx.dir.path() / L"resume.pdf"));
}

TEST_CASE("executor moves unicode, compound, dotfile and extensionless names") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-names"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"\x6587\x4EF6.txt", "cjk"));
    REQUIRE(fixture::create_file(fx.dir.path(), L"backup.tar.gz", "tar"));
    REQUIRE(fixture::create_file(fx.dir.path(), L".gitignore", "dot"));
    REQUIRE(fixture::create_file(fx.dir.path(), L"README", "readme"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());
    REQUIRE(fx.plan.planned_count() == 4);

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    CHECK(result.moved_count == 4);
    CHECK(std::filesystem::exists(fx.dir.path() / L"Documents" / L"\x6587\x4EF6.txt"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"Archives" / L"backup.tar.gz"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"Other" / L".gitignore"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"Other" / L"README"));
    CHECK(file_exists_with_content(fx.dir.path() / L"Documents" / L"\x6587\x4EF6.txt", "cjk"));
}

TEST_CASE("executor reports a conflict when the destination exists and preserves both sides") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-conflict"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"resume.pdf", "incoming"));
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    REQUIRE(fixture::create_file(fx.dir.path() / L"Documents", L"resume.pdf", "existing"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());
    // Planning already resolved the collision: the plan says "resume (2).pdf".
    REQUIRE(fx.plan.planned_count() == 1);
    CHECK(fx.plan.moves()[0].destination_name == "resume (2).pdf");

    // Simulate a destination appearing AFTER planning under the planned name.
    REQUIRE(fixture::create_file(fx.dir.path() / L"Documents", L"resume (2).pdf", "late"));

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::Conflict);
    CHECK(result.conflict_count == 1);
    CHECK(result.moved_count == 0);
    // Source untouched, late destination content byte-identical.
    CHECK(file_exists_with_content(fx.dir.path() / L"resume.pdf", "incoming"));
    CHECK(file_exists_with_content(fx.dir.path() / L"Documents" / L"resume (2).pdf", "late"));
    CHECK(file_exists_with_content(fx.dir.path() / L"Documents" / L"resume.pdf", "existing"));
}

TEST_CASE("executor rejects a source replaced after planning") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-replaced"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"note.txt", "original"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // Replace the source with a GENUINELY DIFFERENT regular file after
    // planning. fixture::create_file alone is NOT a replacement: it opens
    // with CREATE_ALWAYS, which truncates the existing file in place and
    // keeps the same NTFS file id (verified via fsutil in Phase 5 runtime
    // verification) - the executor would then see an unchanged identity and
    // move the file, per the ratified identity-not-content policy. A real
    // replacement is delete + recreate, and this test PROVES the identity
    // changed before asserting the executor detects it.
    const auto id_before =
        fixture::file_id_string(fx.dir.path() / L"note.txt");
    REQUIRE_FALSE(id_before.empty());
    REQUIRE(::DeleteFileW((fx.dir.path() / L"note.txt").c_str()) != 0);
    REQUIRE(fixture::create_file(fx.dir.path(), L"note.txt", "REPLACED"));
    const auto id_after =
        fixture::file_id_string(fx.dir.path() / L"note.txt");
    REQUIRE_FALSE(id_after.empty());
    REQUIRE(id_before != id_after); // the identity genuinely changed

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::SourceReplaced);
    CHECK(result.moved_count == 0);
    // The replacement was NOT moved.
    CHECK(std::filesystem::exists(fx.dir.path() / L"note.txt"));
}

TEST_CASE("executor reports a source that vanished after planning") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-vanished"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"ghost.txt", "boo"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    REQUIRE(::DeleteFileW((fx.dir.path() / L"ghost.txt").c_str()) != 0);

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::SourceMissing);
    CHECK(result.moved_count == 0);
}

TEST_CASE("executor keeps a content-modified file (identity-not-content policy)") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-edited"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"diary.txt", "v1"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // Same file, different content: identity is unchanged, so it still moves.
    REQUIRE(fixture::create_file(fx.dir.path(), L"diary.txt", "v2-with-more-text"));

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::Moved);
    CHECK(file_exists_with_content(fx.dir.path() / L"Documents" / L"diary.txt",
                                   "v2-with-more-text"));
}

TEST_CASE("executor reports a locked source and continues independent moves") {
    if (!fixture::root_relative_rename_supported()) {
        MESSAGE(fixture::kMovementSkipReason);
        return;
    }
    PlannedFixture fx{"exec-locked"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"locked.txt", "busy"));
    REQUIRE(fixture::create_file(fx.dir.path(), L"free.txt", "free"));

    // Scan and plan FIRST: the scanner itself opens candidates DELETE-capably
    // and would (correctly) skip an already-locked entry, so the executor
    // could never see it. The executor's Locked outcome models a lock that
    // appears BETWEEN planning and execution - which is exactly the sequence
    // exercised here.
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // A DELETE-capable handle without FILE_SHARE_DELETE: the executor's
    // fresh source open must fail closed with a sharing violation.
    HANDLE raw = ::CreateFileW((fx.dir.path() / L"locked.txt").c_str(), DELETE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    // RAII: the handle is released on every path (including REQUIRE aborts),
    // so fixture cleanup can always delete the directory.
    platform::unique_handle held{raw};

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 2);
    const auto* locked = find_record(result, "locked.txt");
    REQUIRE(locked != nullptr);
    CHECK(locked->outcome == MoveOutcome::Locked);
    CHECK(result.moved_count == 1);
    CHECK(result.failed_count == 1);
    // The locked file is still in the root, untouched.
    CHECK(file_exists_with_content(fx.dir.path() / L"locked.txt", "busy"));
}

TEST_CASE("executor aborts before any mutation when the root changed") {
    PlannedFixture fx{"exec-root-a"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"resume.pdf", "r"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // Execute against a DIFFERENT directory: the fresh bootstrap yields a
    // different root identity -> batch abort before any mutation.
    SandboxDir other{"exec-root-b", SandboxRoot::Phase5};
    REQUIRE(fixture::create_file(other.path(), L"resume.pdf", "other"));

    const auto result = execute_plan(fx.plan, other.u8());
    CHECK(result.aborted);
    CHECK(result.abort_reason.find("RootChanged") != std::string::npos);
    CHECK(result.records.empty());
    // Nothing moved in either directory.
    CHECK(std::filesystem::exists(fx.dir.path() / L"resume.pdf"));
    CHECK(std::filesystem::exists(other.path() / L"resume.pdf"));
    CHECK_FALSE(std::filesystem::exists(other.path() / L"Documents"));
}

TEST_CASE("executor aborts on an invalid plan before any mutation") {
    PlannedFixture fx{"exec-invalid"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"resume.pdf", "r"));
    auto scan = scan_directory(fx.guard.root);
    scan.enumeration = EnumerationStatus::Failed; // inject an invalid scan

    const auto plan = build_plan(fx.guard.root, scan);
    CHECK_FALSE(plan.valid());
    const auto result = execute_plan(plan, fx.dir.u8());
    CHECK(result.aborted);
    CHECK(result.records.empty());
    CHECK(std::filesystem::exists(fx.dir.path() / L"resume.pdf"));
}

TEST_CASE("executor rejects a category directory replaced after planning") {
    PlannedFixture fx{"exec-catreplaced"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"doc.pdf", "d"));
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    // Plan while Documents exists (identity recorded).
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // Replace Documents with a FRESH empty directory after planning: the
    // executor must detect the identity change and never adopt it.
    REQUIRE(::RemoveDirectoryW((fx.dir.path() / L"Documents").c_str()) != 0);
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    const auto result = execute_plan(fx.plan, fx.dir.u8());
    // The fresh directory has a DIFFERENT identity -> DestinationChanged.
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::DestinationChanged);
    CHECK(result.moved_count == 0);
    CHECK(std::filesystem::exists(fx.dir.path() / L"doc.pdf"));
}

TEST_CASE("executor reports a category that was deleted after planning") {
    PlannedFixture fx{"exec-catdeleted"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"doc.pdf", "d"));
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // Delete Documents after planning; the executor must NOT adopt a
    // replacement and must NOT create it (the plan said it exists).
    REQUIRE(::RemoveDirectoryW((fx.dir.path() / L"Documents").c_str()) != 0);

    const auto result = execute_plan(fx.plan, fx.dir.u8());
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].outcome == MoveOutcome::DestinationChanged);
    CHECK(result.moved_count == 0);
    CHECK_FALSE(std::filesystem::exists(fx.dir.path() / L"Documents"));
    CHECK(std::filesystem::exists(fx.dir.path() / L"doc.pdf"));
}

TEST_CASE("rename primitive rejects invalid destination components up front") {
    PlannedFixture fx{"exec-badname"};
    REQUIRE(fx.guard.ok);
    REQUIRE(fixture::create_file(fx.dir.path(), L"doc.pdf", "d"));
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    // The rename primitive must refuse dangerous components BEFORE any OS
    // call, regardless of what the plan contains (defense against planner
    // bugs). Drive the primitive directly through the platform layer.
    HANDLE source = ::CreateFileW((fx.dir.path() / L"doc.pdf").c_str(),
                                  DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(source != INVALID_HANDLE_VALUE);

    auto category = platform::open_category_directory(fx.guard.root.get(), "Documents", false);
    REQUIRE(category.ok);

    const std::string bad_names[] = {"", ".", "..", "a\\b.pdf", "a/b.pdf", "a:b.pdf",
                                     "..\\x.pdf", std::string(256, 'x')};
    for (const std::string& bad : bad_names) {
        const auto r = platform::rename_to_category(source, category.handle.get(), bad);
        CHECK_FALSE(r.ok);
        CHECK(r.failure == platform::NativeFailure::InvalidPath);
    }
    // The source is untouched after all refusals.
    CHECK(std::filesystem::exists(fx.dir.path() / L"doc.pdf"));
    ::CloseHandle(source);
}

TEST_CASE("controlled A/B: Win32 class-3 vs native class-10 anchored rename") {
    PlannedFixture fx{"exec-ab"};
    REQUIRE(fx.guard.ok);
    // Six identical source files: two for A, two for B, two spare.
    for (int i = 0; i < 6; ++i) {
        wchar_t name[32];
        std::swprintf(name, 32, L"ab-%02d.txt", i);
        REQUIRE(fixture::create_file(fx.dir.path(), name, "content-" + std::to_string(i)));
    }
    REQUIRE(fixture::create_directory(fx.dir.path(), L"Documents"));
    fx.scan = scan_directory(fx.guard.root);
    fx.plan = build_plan(fx.guard.root, fx.scan);
    REQUIRE(fx.plan.valid());

    auto cat = platform::open_category_directory(fx.guard.root.get(), "Documents", false);
    REQUIRE(cat.ok);

    // A: Win32 class 3 (expected FAIL error 87 on build 26300.9711).
    {
        auto src = platform::open_source(fx.guard.root.get(), "ab-00.txt");
        REQUIRE(src.ok);
        const auto r = platform::win32_rename_to_category(
            src.handle.get(), cat.handle.get(), "win32-moved.txt");
        MESSAGE("A win32 -> ", r.ok ? "SUCCESS" : "FAIL",
                r.ok ? "" : " error " + std::to_string(r.native_code));
        CHECK_FALSE(r.ok); // documented blocked path on this OS build
        CHECK(r.native_code == 87);
    }
    // B: native class 10 (must SUCCEED - the decision gate).
    {
        auto src = platform::open_source(fx.guard.root.get(), "ab-01.txt");
        REQUIRE(src.ok);
        const auto r = platform::native_rename_to_category(
            src.handle.get(), cat.handle.get(), "native-moved.txt");
        REQUIRE(r.ok);
        CHECK(r.native_code == 0); // NTSTATUS success, IoStatusBlock.Status 0
    }
    // B: unicode destination (narrow UTF-8 parameter, wide existence check).
    {
        auto src = platform::open_source(fx.guard.root.get(), "ab-02.txt");
        REQUIRE(src.ok);
        const auto r = platform::native_rename_to_category(
            src.handle.get(), cat.handle.get(),
            "\xE6\x96\x87\xE4\xBB\xB6.txt"); // 文件.txt
        REQUIRE(r.ok);
        CHECK(std::filesystem::exists(fx.dir.path() / L"Documents" /
                                      L"\x6587\x4EF6.txt"));
    }
    // B: existing destination fails safely, both sides preserved.
    {
        REQUIRE(fixture::create_file(fx.dir.path() / L"Documents", L"native-conflict.txt",
                                     "existing"));
        auto src = platform::open_source(fx.guard.root.get(), "ab-03.txt");
        REQUIRE(src.ok);
        const auto r = platform::native_rename_to_category(
            src.handle.get(), cat.handle.get(), "native-conflict.txt");
        CHECK_FALSE(r.ok);
        CHECK(r.failure == platform::NativeFailure::Conflict);
        CHECK(fixture::file_content_matches(
            fx.dir.path() / L"Documents" / L"native-conflict.txt", "existing"));
        CHECK(fixture::file_content_matches(fx.dir.path() / L"ab-03.txt", "content-3"));
    }
    // B: long valid filename (255 UTF-16 code units).
    {
        auto src = platform::open_source(fx.guard.root.get(), "ab-04.txt");
        REQUIRE(src.ok);
        const std::string long_name(251, 'x'); // + ".txt" = 255 units
        const auto r = platform::native_rename_to_category(
            src.handle.get(), cat.handle.get(), long_name + ".txt");
        REQUIRE(r.ok);
        // The destination's full path exceeds MAX_PATH: plain std::filesystem
        // calls cannot see it. Use the extended-length (\\?\) helpers.
        CHECK(fixture::file_exists_extended(fx.dir.path() / L"Documents" /
                                            (long_name + ".txt")));
        CHECK(fixture::file_content_matches_extended(
            fx.dir.path() / L"Documents" / (long_name + ".txt"), "content-4"));
    }
    // B: missing RootDirectory fails closed.
    {
        auto src = platform::open_source(fx.guard.root.get(), "ab-05.txt");
        REQUIRE(src.ok);
        const auto r = platform::native_rename_to_category(src.handle.get(), nullptr,
                                                           "orphan.txt");
        CHECK_FALSE(r.ok);
        CHECK(r.failure == platform::NativeFailure::Other);
    }
}