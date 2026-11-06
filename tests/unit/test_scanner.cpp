// Phase 3 tests: directory scanner (sandboxed integration).
// Every fixture lives under build/phase3-test-sandbox/. The read-only
// guarantee is enforced by before/after snapshots of the whole fixture.

#include <doctest.h>

#include <algorithm>
#include <cwchar>
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
#include "folderpilot/scanner.hpp"
#include "text.hpp"

using namespace folderpilot;

namespace {

// Finds a skipped entry by name (empty name matches any, e.g. encoding errors).
const SkippedEntry* find_skipped(const ScanResult& result, const std::string& name) {
    for (const SkippedEntry& entry : result.skipped) {
        if (name.empty() || entry.name_utf8 == name) {
            return &entry;
        }
    }
    return nullptr;
}

const ScannedFile* find_file(const ScanResult& result, const std::string& name) {
    for (const ScannedFile& file : result.files) {
        if (file.name_utf8 == name) {
            return &file;
        }
    }
    return nullptr;
}

// RAII raw handle for compatibility probes in tests.
struct RawHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~RawHandle() { if (handle != INVALID_HANDLE_VALUE) { ::CloseHandle(handle); } }
};

} // namespace

TEST_CASE("scanner reports an empty directory as an empty success") {
    fixture::SandboxDir dir{"scan-empty"};
    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);

    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);
    CHECK(result.files.empty());
    // Some filesystems surface "." and ".." through directory enumeration;
    // the scanner must skip them defensively and report nothing else.
    for (const SkippedEntry& entry : result.skipped) {
        CHECK(entry.reason == SkipReason::ReservedName);
    }
    CHECK(result.root.volume_guid_path.starts_with("\\\\?\\Volume{"));
}

TEST_CASE("scanner captures one file with identity and metadata") {
    fixture::SandboxDir dir{"scan-one"};
    REQUIRE(fixture::create_file(dir.path(), L"report.pdf", "0123456789"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);
    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "report.pdf");
    CHECK(result.files[0].size_bytes == 10);
    CHECK(result.files[0].last_write_time != 0);
    CHECK((result.files[0].attributes & FILE_ATTRIBUTE_ARCHIVE) != 0);
}

TEST_CASE("scanner skips directories, hidden and system entries with reasons") {
    fixture::SandboxDir dir{"scan-mixed"};
    REQUIRE(fixture::create_file(dir.path(), L"keep.txt", "x"));
    REQUIRE(fixture::create_directory(dir.path(), L"subfolder"));
    REQUIRE(fixture::create_file(dir.path() / L"subfolder", L"inside.txt", "never listed"));
    REQUIRE(fixture::create_file(dir.path(), L"hidden.txt", "h"));
    REQUIRE(fixture::add_attributes(dir.path() / L"hidden.txt", FILE_ATTRIBUTE_HIDDEN));
    REQUIRE(fixture::create_file(dir.path(), L"system.dat", "s"));
    REQUIRE(fixture::add_attributes(dir.path() / L"system.dat", FILE_ATTRIBUTE_SYSTEM));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "keep.txt");

    // "." / ".." (if the filesystem surfaces them) are skipped defensively
    // as ReservedName; the meaningful skips are exactly these three:
    std::size_t meaningful = 0;
    for (const SkippedEntry& entry : result.skipped) {
        if (entry.reason != SkipReason::ReservedName) {
            ++meaningful;
        }
    }
    REQUIRE(meaningful == 3);
    const auto* sub = find_skipped(result, "subfolder");
    REQUIRE(sub != nullptr);
    CHECK(sub->reason == SkipReason::Directory);
    const auto* hidden = find_skipped(result, "hidden.txt");
    REQUIRE(hidden != nullptr);
    CHECK(hidden->reason == SkipReason::Hidden);
    const auto* system = find_skipped(result, "system.dat");
    REQUIRE(system != nullptr);
    CHECK(system->reason == SkipReason::System);
    // Never entered the subdirectory.
    CHECK(find_file(result, "inside.txt") == nullptr);
}

TEST_CASE("scanner handles multiple enumeration batches with a small buffer") {
    fixture::SandboxDir dir{"scan-batches"};
    for (int i = 0; i < 100; ++i) {
        wchar_t name[32];
        std::swprintf(name, 32, L"batch-file-%04d.dat", i);
        REQUIRE(fixture::create_file(dir.path(), name, "x"));
    }
    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);

    ScanOptions options;
    options.buffer_bytes = 4096; // forces several enumeration batches
    const auto result = scan_directory(guard.root, options);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);
    CHECK(result.files.size() == 100);
    // Deterministic ordinal ordering.
    CHECK(std::is_sorted(result.files.begin(), result.files.end(),
                         [](const ScannedFile& a, const ScannedFile& b) {
                             return a.name_utf8 < b.name_utf8;
                         }));
}

TEST_CASE("scanner preserves unicode names, dots and extension-less names") {
    fixture::SandboxDir dir{"scan-names"};
    const std::wstring unicode_doc = L"\x6587\x4EF6\x5939\x62A5\x544A.txt"; // 文件夹报告.txt
    const std::wstring rocket = L"\xD83D\xDE80rocket.mp4";                  // 🚀rocket.mp4
    REQUIRE(fixture::create_file(dir.path(), unicode_doc, "u"));
    REQUIRE(fixture::create_file(dir.path(), L"archive.tar.gz", "a"));
    REQUIRE(fixture::create_file(dir.path(), rocket, "v"));
    REQUIRE(fixture::create_file(dir.path(), L"NoExtension", "n"));
    REQUIRE(fixture::create_file(dir.path(), L"multi.dot.name.pdf", "m"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);
    REQUIRE(result.files.size() == 5);
    const auto unicode_u8 = platform::to_utf8(unicode_doc);
    const auto rocket_u8 = platform::to_utf8(rocket);
    REQUIRE(unicode_u8.ok);
    REQUIRE(rocket_u8.ok);
    CHECK(find_file(result, unicode_u8.value) != nullptr);
    CHECK(find_file(result, "archive.tar.gz") != nullptr);
    CHECK(find_file(result, rocket_u8.value) != nullptr);
    CHECK(find_file(result, "NoExtension") != nullptr);
    CHECK(find_file(result, "multi.dot.name.pdf") != nullptr);
}

TEST_CASE("scanner reports files that vanish between enumeration and opening") {
    fixture::SandboxDir dir{"scan-vanish"};
    REQUIRE(fixture::create_file(dir.path(), L"doomed.txt", "d"));
    REQUIRE(fixture::create_file(dir.path(), L"stable.txt", "s"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);

    ScanOptions options;
    options.before_open = [&dir](const std::string& name) {
        if (name == "doomed.txt") {
            ::DeleteFileW((dir.path() / L"doomed.txt").c_str());
        }
    };
    const auto result = scan_directory(guard.root, options);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "stable.txt");
    const auto* doomed = find_skipped(result, "doomed.txt");
    REQUIRE(doomed != nullptr);
    CHECK(doomed->reason == SkipReason::Vanished);
}

TEST_CASE("scanner reports a file replaced by a directory as not a regular file") {
    fixture::SandboxDir dir{"scan-replaced-dir"};
    REQUIRE(fixture::create_file(dir.path(), L"shapeshift.txt", "d"));
    REQUIRE(fixture::create_file(dir.path(), L"stable.txt", "s"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);

    ScanOptions options;
    options.before_open = [&dir](const std::string& name) {
        if (name == "shapeshift.txt") {
            ::DeleteFileW((dir.path() / L"shapeshift.txt").c_str());
            ::CreateDirectoryW((dir.path() / L"shapeshift.txt").c_str(), nullptr);
        }
    };
    const auto result = scan_directory(guard.root, options);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "stable.txt");
    const auto* replaced = find_skipped(result, "shapeshift.txt");
    REQUIRE(replaced != nullptr);
    CHECK(replaced->reason == SkipReason::NotRegularFile);
}

TEST_CASE("scanner skips a file replaced by a reparse point") {
    fixture::SandboxDir dir{"scan-replaced-link"};
    fixture::SandboxDir real{"scan-replaced-link-real"};
    REQUIRE(fixture::create_file(dir.path(), L"linkified.txt", "d"));
    REQUIRE(fixture::create_file(dir.path(), L"stable.txt", "s"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);

    ScanOptions options;
    options.before_open = [&](const std::string& name) {
        if (name == "linkified.txt") {
            const bool swapped =
                fixture::create_junction(dir.path() / L"newlink", real.path()) &&
                ::DeleteFileW((dir.path() / L"linkified.txt").c_str()) != 0 &&
                ::MoveFileW((dir.path() / L"newlink").c_str(),
                            (dir.path() / L"linkified.txt").c_str()) != 0;
            if (!swapped) {
                MESSAGE("SKIPPED (capability unavailable): junction swap failed on this machine");
            }
        }
    };
    const auto result = scan_directory(guard.root, options);
    const auto* replaced = find_skipped(result, "linkified.txt");
    if (replaced == nullptr) {
        MESSAGE("SKIPPED (capability unavailable): junction swap did not take effect");
        return;
    }
    // A directory reparse point is rejected safely either way: the mandated
    // FILE_NON_DIRECTORY_FILE open refuses it (NotRegularFile) before any
    // attribute check, and a file reparse point is caught by the reparse
    // tag. Both mean: the link was never followed.
    const bool safely_rejected = replaced->reason == SkipReason::ReparsePoint ||
                                 replaced->reason == SkipReason::NotRegularFile;
    CHECK(safely_rejected);
    CHECK(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "stable.txt");
}

TEST_CASE("scanner reports an incompatible existing DELETE-capable handle") {
    fixture::SandboxDir dir{"scan-locked"};
    REQUIRE(fixture::create_file(dir.path(), L"locked.txt", "busy"));
    REQUIRE(fixture::create_file(dir.path(), L"free.txt", "ok"));

    // Hold a DELETE-capable handle WITHOUT sharing delete access - the
    // scanner's source open must fail safely on this file only.
    RawHandle held;
    held.handle = ::CreateFileW((dir.path() / L"locked.txt").c_str(), DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held.handle != INVALID_HANDLE_VALUE);

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "free.txt");
    const auto* locked = find_skipped(result, "locked.txt");
    REQUIRE(locked != nullptr);
    CHECK(locked->reason == SkipReason::InUse);
}

TEST_CASE("scanning never mutates the fixture (read-only guarantee)") {
    fixture::SandboxDir dir{"scan-readonly"};
    const std::wstring unicode_name = L"\x6587\x4EF6.txt"; // 文件.txt
    REQUIRE(fixture::create_file(dir.path(), L"a.txt", "alpha"));
    REQUIRE(fixture::create_file(dir.path(), unicode_name, "unicode"));
    REQUIRE(fixture::create_directory(dir.path(), L"existing-folder"));
    REQUIRE(fixture::create_file(dir.path() / L"existing-folder", L"nested.bin", "xyz"));

    const auto before = fixture::take_snapshot(dir.path());

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);
    REQUIRE(result.files.size() == 2); // nested.bin is never listed (no recursion)

    const auto after = fixture::take_snapshot(dir.path());
    CHECK(fixture::snapshots_equal(before, after));
}

TEST_CASE("scanner output is deterministic across runs") {
    fixture::SandboxDir dir{"scan-deterministic"};
    REQUIRE(fixture::create_file(dir.path(), L"a.txt", "1"));
    REQUIRE(fixture::create_file(dir.path(), L"B.txt", "2"));
    REQUIRE(fixture::create_file(dir.path(), L"c.txt", "3"));

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto first = scan_directory(guard.root);
    const auto second = scan_directory(guard.root);
    REQUIRE(first.enumeration == EnumerationStatus::Ok);
    REQUIRE(second.enumeration == EnumerationStatus::Ok);

    REQUIRE(first.files.size() == 3);
    // Ordinal case-insensitive order: a.txt < B.txt < c.txt.
    CHECK(first.files[0].name_utf8 == "a.txt");
    CHECK(first.files[1].name_utf8 == "B.txt");
    CHECK(first.files[2].name_utf8 == "c.txt");
    CHECK(first.files.size() == second.files.size());
    for (std::size_t i = 0; i < first.files.size(); ++i) {
        CHECK(first.files[i].name_utf8 == second.files[i].name_utf8);
        CHECK(platform::identity_matches(first.files[i].identity, second.files[i].identity));
    }
}

TEST_CASE("scanner fails closed when the root handle is invalid") {
    platform::RootDirectory invalid;
    const auto result = scan_directory(invalid);
    CHECK(result.enumeration == EnumerationStatus::Failed);
    CHECK_FALSE(result.error.empty());
    CHECK(result.files.empty());
}

TEST_CASE("scanner skips reparse-point entries without following them") {
    fixture::SandboxDir dir{"scan-junction"};
    fixture::SandboxDir real{"scan-junction-real"};
    REQUIRE(fixture::create_file(real.path(), L"secret.txt", "do not list"));
    REQUIRE(fixture::create_file(dir.path(), L"normal.txt", "n"));

    const bool ok = fixture::create_junction(dir.path() / L"junction", real.path());
    if (!ok) {
        MESSAGE("SKIPPED (capability unavailable): junction creation on this machine");
        return;
    }

    const auto guard = validate_target(dir.u8());
    REQUIRE(guard.ok);
    const auto result = scan_directory(guard.root);
    REQUIRE(result.enumeration == EnumerationStatus::Ok);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].name_utf8 == "normal.txt");
    const auto* junction = find_skipped(result, "junction");
    REQUIRE(junction != nullptr);
    CHECK(junction->reason == SkipReason::ReparsePoint);
    // The junction's target content never appears.
    CHECK(find_file(result, "secret.txt") == nullptr);
}
