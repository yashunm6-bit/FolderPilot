// Phase 3 unit tests: pure platform helpers (no filesystem access).

#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include "native.hpp"
#include "text.hpp"

using namespace folderpilot::platform;

TEST_CASE("classify_dos_path accepts absolute local drive paths only") {
    CHECK_FALSE(classify_dos_path(L"C:\\").has_value());
    CHECK_FALSE(classify_dos_path(L"C:\\Users\\test\\folder").has_value());
    CHECK_FALSE(classify_dos_path(L"c:\\lower\\drive").has_value());
    CHECK_FALSE(classify_dos_path(L"C:\\\x00E9\x6587\x4EF6\\unicode").has_value()); // é文...

    // Relative paths.
    CHECK(classify_dos_path(L"folder") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L".\\folder") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"..\\folder") == NativeFailure::InvalidPath);
    // Root-relative (current drive).
    CHECK(classify_dos_path(L"\\folder") == NativeFailure::InvalidPath);
    // Drive-relative.
    CHECK(classify_dos_path(L"C:folder") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"C:") == NativeFailure::InvalidPath);
    // UNC and device namespaces.
    CHECK(classify_dos_path(L"\\\\server\\share") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"\\\\.\\C:") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"\\\\?\\C:\\folder") == NativeFailure::InvalidPath);
    // Alternate data streams.
    CHECK(classify_dos_path(L"C:\\file.txt:stream") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"C:\\file:ads.txt") == NativeFailure::InvalidPath);
    // Other unsupported forms.
    CHECK(classify_dos_path(L"C:/forward/slashes") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"") == NativeFailure::InvalidPath);
    CHECK(classify_dos_path(L"1:\\not\\a\\drive") == NativeFailure::InvalidPath);
    // Embedded NUL: a path that silently truncates at the null must be rejected.
    std::wstring truncated = L"C:\\good";
    truncated.push_back(L'\0');
    truncated += L"evil";
    CHECK(classify_dos_path(truncated) == NativeFailure::InvalidPath);
}

TEST_CASE("compare_ordinal_ci is ordinal and case-insensitive") {
    CHECK(compare_ordinal_ci("ABC", "abc") == 0);
    CHECK(compare_ordinal_ci("Documents", "DOCUMENTS") == 0);
    CHECK(compare_ordinal_ci("a", "B") < 0);
    CHECK(compare_ordinal_ci("B", "a") > 0);
    CHECK(compare_ordinal_ci("Users", "users") == 0);
    CHECK(compare_ordinal_ci("$Recycle.Bin", "$recycle.bin") == 0);
    // Unicode case folding through CompareStringOrdinal.
    CHECK(compare_ordinal_ci("\xC3\x84nderungen", "\xC3\xA4nderungen") == 0); // Ä / ä
    CHECK(compare_ordinal_ci("报告", "报告") == 0);
    CHECK(compare_ordinal_ci("abc", "abd") < 0);
}

TEST_CASE("identity representations never compare across variants") {    FILE_ID_128 id_a{}; id_a.Identifier[0] = 1;
    FILE_ID_128 id_b{}; id_b.Identifier[0] = 1;
    FILE_ID_128 id_c{}; id_c.Identifier[0] = 2;

    const SourceIdentity ex_a = SourceIdentityEx{42, id_a};
    const SourceIdentity ex_b = SourceIdentityEx{42, id_b};
    const SourceIdentity ex_c = SourceIdentityEx{42, id_c};
    const SourceIdentity ex_other_volume = SourceIdentityEx{43, id_a};
    const SourceIdentity legacy = SourceIdentityLegacy{42, 0, 1};

    CHECK(identity_matches(ex_a, ex_b));
    CHECK_FALSE(identity_matches(ex_a, ex_c));
    CHECK_FALSE(identity_matches(ex_a, ex_other_volume));
    CHECK(identity_matches(legacy, legacy));
    CHECK_FALSE(identity_matches(SourceIdentityLegacy{42, 0, 2}, legacy));
    // Representation mismatch: fail closed even with identical-looking numbers.
    CHECK_FALSE(identity_matches(ex_a, legacy));
    CHECK_FALSE(identity_matches(legacy, ex_a));
}

namespace {

// Builds one valid FILE_ID_BOTH_DIR_INFO entry (optionally chained) as the
// OS would lay it out. Uses offsetof() so the test tracks the real header.
std::vector<std::byte> build_entry(std::wstring_view name, unsigned long next_entry) {
    constexpr std::size_t kHeader = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    const std::size_t name_bytes = name.size() * 2;
    std::vector<std::byte> buffer(kHeader + name_bytes);

    FILE_ID_BOTH_DIR_INFO info{};
    info.NextEntryOffset = next_entry;
    info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    info.EndOfFile.QuadPart = 1234;
    info.LastWriteTime.QuadPart = 5678;
    info.FileId.QuadPart = 0xABCDEF;
    info.FileNameLength = static_cast<unsigned long>(name_bytes);

    std::memcpy(buffer.data(), &info, kHeader);
    std::memcpy(buffer.data() + kHeader, name.data(), name_bytes);
    return buffer;
}

std::size_t align8(std::size_t value) {
    return (value + 7) / 8 * 8;
}

} // namespace

TEST_CASE("volume mount-point normalization leaves exactly one trailing backslash") {
    CHECK(normalize_volume_mount_point(L"C:\\") == L"C:\\");
    CHECK(normalize_volume_mount_point(L"C:") == L"C:\\");
    CHECK(normalize_volume_mount_point(L"C:\\\\\\\\") == L"C:\\"); // many trailing
    CHECK(normalize_volume_mount_point(L"D:\\mounts\\data") == L"D:\\mounts\\data\\");
    CHECK(normalize_volume_mount_point(L"D:\\mounts\\data\\\\") == L"D:\\mounts\\data\\");
    CHECK(normalize_volume_mount_point(L"") == L"\\");
    // Idempotent: normalizing twice is the same as normalizing once.
    const std::wstring once = normalize_volume_mount_point(L"E:\\x\\\\");
    CHECK(normalize_volume_mount_point(once) == once);
}

TEST_CASE("volume identity fails closed for nonexistent paths") {
    VolumeIdentity out;
    const auto result = volume_identity_for_path(L"Q:\\definitely-not-here-98765", out);
    CHECK_FALSE(result.ok);
    CHECK(!result.error.empty());
    CHECK((result.failure == NativeFailure::Other || result.failure == NativeFailure::Unsupported));
}

TEST_CASE("directory buffer parser accepts a single valid entry") {
    const auto buffer = build_entry(L"report.pdf", 0);
    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 1);
    CHECK(parsed.entries[0].name == L"report.pdf");
    CHECK(parsed.entries[0].end_of_file == 1234);
    CHECK(parsed.entries[0].last_write_time == 5678);
    CHECK(parsed.entries[0].file_id == 0xABCDEF);
    CHECK(parsed.entries[0].attributes == FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE("directory buffer parser walks a chained batch") {
    const auto first = build_entry(L"one.txt", 0);
    const auto second = build_entry(L"two.txt", 0);
    const unsigned long stride = static_cast<unsigned long>(align8(first.size()));

    std::vector<std::byte> buffer(stride + second.size());
    std::memcpy(buffer.data(), first.data(), first.size());
    std::memcpy(buffer.data() + stride, second.data(), second.size());
    // Patch the chain: first entry points at the second.
    *reinterpret_cast<unsigned long*>(buffer.data()) = stride;

    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].name == L"one.txt");
    CHECK(parsed.entries[1].name == L"two.txt");
}

TEST_CASE("directory buffer parser accepts an empty batch") {
    const auto parsed = parse_id_both_dir_entries(nullptr, 0);
    CHECK(parsed.status == DirParseStatus::Ok);
    CHECK(parsed.entries.empty());
}

TEST_CASE("directory buffer parser preserves dot and dot-dot entries verbatim") {
    // Some filesystems surface "." and ".." through enumeration; the parser
    // must hand them through unchanged so the scanner can skip them.
    const auto dot = build_entry(L".", 0);
    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(dot.data()), dot.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 1);
    CHECK(parsed.entries[0].name == L".");

    const auto dotdot = build_entry(L"..", 0);
    const auto parsed2 = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(dotdot.data()), dotdot.size());
    REQUIRE(parsed2.status == DirParseStatus::Ok);
    REQUIRE(parsed2.entries.size() == 1);
    CHECK(parsed2.entries[0].name == L"..");
}

TEST_CASE("directory buffer parser keeps duplicate names (no deduplication)") {
    // Two entries with the SAME name must both survive - deduplication by
    // filename is explicitly forbidden.
    const auto first = build_entry(L"same.txt", 0);
    const auto second = build_entry(L"same.txt", 0);
    const unsigned long stride = static_cast<unsigned long>(align8(first.size()));

    std::vector<std::byte> buffer(stride + second.size());
    std::memcpy(buffer.data(), first.data(), first.size());
    std::memcpy(buffer.data() + stride, second.data(), second.size());
    *reinterpret_cast<unsigned long*>(buffer.data()) = stride;

    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].name == L"same.txt");
    CHECK(parsed.entries[1].name == L"same.txt");
}

TEST_CASE("directory buffer parser accepts a name ending exactly at the buffer edge") {
    // The name region may legitimately end at the last valid byte; the
    // bounds check must be inclusive.
    constexpr std::size_t kHeader = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    const std::size_t name_bytes = 8; // L"edge" (4 chars)
    std::vector<std::byte> buffer(kHeader + name_bytes);

    FILE_ID_BOTH_DIR_INFO info{};
    info.NextEntryOffset = 0;
    info.FileNameLength = static_cast<unsigned long>(name_bytes);
    std::memcpy(buffer.data(), &info, kHeader);
    std::memcpy(buffer.data() + kHeader, L"edge", name_bytes);

    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 1);
    CHECK(parsed.entries[0].name == L"edge");
}

TEST_CASE("invalid UTF-16 in an entry name fails the strict conversion") {
    // The OS never hands out invalid UTF-16 names, but the conversion the
    // scanner relies on must still be proven to reject one rather than
    // repair it. A lone surrogate is the canonical invalid case.
    const auto entry = build_entry(std::wstring{L'a', L'\xD800', L'b'}, 0);
    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(entry.data()), entry.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 1);

    const auto converted = to_utf8(parsed.entries[0].name);
    CHECK_FALSE(converted.ok);
    CHECK(!converted.error.empty());
}

TEST_CASE("directory buffer parser rejects malformed entries") {
    constexpr std::size_t kHeader = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);

    SUBCASE("odd FileNameLength") {
        auto buffer = build_entry(L"odd", 0);
        unsigned long bad = 5; // not a whole number of UTF-16 units
        std::memcpy(buffer.data() + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), &bad, 4);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::BadNameLength);
    }
    SUBCASE("filename extends past the buffer") {
        auto buffer = build_entry(L"abc", 0);
        unsigned long huge = 100000;
        std::memcpy(buffer.data() + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), &huge, 4);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::NameOutOfBounds);
    }
    SUBCASE("misaligned NextEntryOffset") {
        auto buffer = build_entry(L"a", 0);
        unsigned long bad = 12; // not an 8-byte multiple
        std::memcpy(buffer.data(), &bad, 4);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::MisalignedEntry);
    }
    SUBCASE("NextEntryOffset past the buffer") {
        auto buffer = build_entry(L"a", 0);
        unsigned long huge = 1000000;
        std::memcpy(buffer.data(), &huge, 4);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::EntryOutOfBounds);
    }
    SUBCASE("truncated header") {
        auto buffer = build_entry(L"abc", 0);
        buffer.resize(kHeader - 10);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::TruncatedHeader);
    }
}
