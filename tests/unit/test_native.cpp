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

TEST_CASE("no-write response over a previously populated buffer fails closed") {
    // Enumeration buffer reuse scenario: batch 1 populated the buffer, the
    // next call wrote NOTHING (all bytes zero). The parser must report an
    // explicit malformed state - never an apparently valid empty entry.
    auto buffer = build_entry(L"stale.txt", 0);
    std::memset(buffer.data(), 0, buffer.size());

    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    CHECK(parsed.status == DirParseStatus::ZeroLengthName);
    CHECK(parsed.entries.empty());
}

TEST_CASE("successful response with an empty first entry is rejected") {
    std::vector<std::byte> buffer(offsetof(FILE_ID_BOTH_DIR_INFO, FileName) + 4);
    // All-zero header: NextEntryOffset 0, FileNameLength 0, attributes 0.
    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    CHECK(parsed.status == DirParseStatus::ZeroLengthName);
    CHECK(parsed.entries.empty());
}

TEST_CASE("first-entry validation accepts valid and rejects invalid headers") {
    const auto valid = build_entry(L"ok.txt", 0);
    CHECK_FALSE(validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(valid.data()), valid.size()).has_value());

    // Nothing written: all-zero header region.
    std::vector<std::byte> empty(offsetof(FILE_ID_BOTH_DIR_INFO, FileName) + 8);
    const auto empty_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(empty.data()), empty.size());
    REQUIRE(empty_result.has_value());
    CHECK(empty_result->find("no valid first entry") != std::string::npos);

    // Odd (non-UTF-16) name length.
    auto odd = build_entry(L"abc", 0);
    unsigned long bad_len = 5;
    std::memcpy(odd.data() + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), &bad_len, 4);
    const auto odd_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(odd.data()), odd.size());
    REQUIRE(odd_result.has_value());
    CHECK(odd_result->find("invalid FileNameLength") != std::string::npos);

    // Filename length beyond the 255-UTF-16-unit maximum: the max-length
    // guard rejects BEFORE the buffer-bounds check runs.
    auto huge_len = build_entry(L"abc", 0);
    unsigned long huge = 100000;
    std::memcpy(huge_len.data() + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), &huge, 4);
    const auto huge_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(huge_len.data()), huge_len.size());
    REQUIRE(huge_result.has_value());
    CHECK(huge_result->find("exceeds the maximum name length") != std::string::npos);

    // Filename beyond the buffer: a length within the 255-unit maximum but
    // past this small buffer reaches the buffer-bounds validation.
    auto oob = build_entry(L"abc", 0);
    unsigned long bounds_len = 500; // even, <= 510, but kHeader + 500 > buffer
    std::memcpy(oob.data() + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), &bounds_len, 4);
    const auto oob_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(oob.data()), oob.size());
    REQUIRE(oob_result.has_value());
    CHECK(oob_result->find("extends past the buffer") != std::string::npos);

    // Misaligned NextEntryOffset.
    auto misaligned = build_entry(L"abc", 0);
    unsigned long bad_next = 12;
    std::memcpy(misaligned.data() + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset),
                &bad_next, 4);
    const auto mis_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(misaligned.data()), misaligned.size());
    REQUIRE(mis_result.has_value());
    CHECK(mis_result->find("8-byte aligned") != std::string::npos);

    // NextEntryOffset past the buffer.
    auto far_next = build_entry(L"abc", 0);
    unsigned long far_next_value = 1000000;
    std::memcpy(far_next.data() + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset),
                &far_next_value, 4);
    const auto far_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(far_next.data()), far_next.size());
    REQUIRE(far_result.has_value());
    CHECK(far_result->find("points past the buffer") != std::string::npos);

    // Over-long filename (more than 255 UTF-16 units).
    auto overlong = build_entry(std::wstring(300, L'x'), 0);
    const auto over_result = validate_first_directory_entry(
        reinterpret_cast<const unsigned char*>(overlong.data()), overlong.size());
    REQUIRE(over_result.has_value());
    CHECK(over_result->find("maximum name length") != std::string::npos);
}

TEST_CASE("enumeration completion contract: initial vs continuation queries") {
    // Documented terminal status: always legitimate.
    CHECK(is_legitimate_enumeration_end(ERROR_NO_MORE_FILES, true));
    CHECK(is_legitimate_enumeration_end(ERROR_NO_MORE_FILES, false));

    // ERROR_FILE_NOT_FOUND is NOT a legitimate end: the documented
    // GetFileInformationByHandleEx contract names only ERROR_NO_MORE_FILES,
    // and FolderPilot enumerates ordinary LOCAL directories (share roots
    // and volume roots are rejected) where a NOT_FOUND more plausibly
    // indicates the directory vanished after bootstrap - fail closed.
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_FILE_NOT_FOUND, true));
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_FILE_NOT_FOUND, false));

    // Everything else fails closed on both query kinds.
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_ACCESS_DENIED, true));
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_ACCESS_DENIED, false));
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_INSUFFICIENT_BUFFER, true));
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_WRITE_FAULT, false));
    CHECK_FALSE(is_legitimate_enumeration_end(ERROR_NOT_SUPPORTED, true));
}

TEST_CASE("enumeration batch limit is a documented clean-failure boundary") {
    CHECK(kEnumerationBatchLimit == 100000);
    CHECK_FALSE(enumeration_batch_limit_exceeded(0));
    CHECK_FALSE(enumeration_batch_limit_exceeded(kEnumerationBatchLimit - 1));
    CHECK(enumeration_batch_limit_exceeded(kEnumerationBatchLimit));
    CHECK(enumeration_batch_limit_exceeded(kEnumerationBatchLimit + 1));
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

TEST_CASE("directory buffer parser rejects overlapping NextEntryOffset") {
    // A next-offset that lands INSIDE the current record would parse
    // record bytes as an entry header - malformed, rejected.
    auto buffer = build_entry(L"overlapped-name.txt", 0);
    unsigned long small_next = 8; // aligned but inside this record
    std::memcpy(buffer.data() + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset),
                &small_next, 4);
    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    CHECK(parsed.status == DirParseStatus::OverlappingEntry);
    CHECK(parsed.entries.empty());
}

TEST_CASE("directory buffer parser accepts a tight non-overlapping chain") {
    // next == the current record size (8-aligned) is valid: the next
    // entry starts exactly at the end of this one.
    auto first = build_entry(L"one.txt", 0);
    auto second = build_entry(L"two.txt", 0);
    const unsigned long stride =
        static_cast<unsigned long>((first.size() + 7) / 8 * 8);
    std::vector<std::byte> buffer(stride + second.size());
    std::memcpy(buffer.data(), first.data(), first.size());
    std::memcpy(buffer.data() + stride, second.data(), second.size());
    *reinterpret_cast<unsigned long*>(buffer.data()) = stride;

    const auto parsed = parse_id_both_dir_entries(
        reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    REQUIRE(parsed.status == DirParseStatus::Ok);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].name == L"one.txt");
    CHECK(parsed.entries[1].name == L"two.txt");
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
        // The parser's check order is deliberate: overlap, then alignment,
        // then bounds. An offset must therefore be BEYOND this record to
        // reach the alignment check; a smaller value overlaps and the
        // overlap check fires first (covered by its own test case above).
        unsigned long misaligned = 108; // >= record size (98), 108 % 8 == 4
        std::memcpy(buffer.data(), &misaligned, 4);
        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed.status == DirParseStatus::MisalignedEntry);

        // An ALIGNED offset beyond the buffer passes alignment and reaches
        // the bounds check - documenting the alignment-before-bounds order.
        unsigned long aligned_oob = 112; // 8-aligned, but past the 98-byte buffer
        std::memcpy(buffer.data(), &aligned_oob, 4);
        const auto parsed_oob = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        CHECK(parsed_oob.status == DirParseStatus::EntryOutOfBounds);
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
