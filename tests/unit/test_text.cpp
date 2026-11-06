// Phase 2 unit tests: Windows UTF-8 / UTF-16 conversion utilities.
// Pure in-memory tests: no files, no directories.

#include <doctest.h>

#include <string>
#include <string_view>

#include "text.hpp"

using namespace folderpilot::platform;

namespace {

// Converts utf8 -> utf16 -> utf8 and checks the round trip is byte-exact.
void check_round_trip(const std::string& utf8) {
    const Utf16ConversionResult wide = to_utf16(utf8);
    REQUIRE(wide.ok);
    const Utf8ConversionResult back = to_utf8(wide.value);
    REQUIRE(back.ok);
    CHECK(back.value == utf8);
    CHECK(back.value.size() == utf8.size()); // byte-exact, not just "equal"
}

} // namespace

TEST_CASE("empty input converts successfully in both directions") {
    const Utf16ConversionResult wide = to_utf16("");
    CHECK(wide.ok);
    CHECK(wide.value.empty());

    const Utf8ConversionResult narrow = to_utf8(L"");
    CHECK(narrow.ok);
    CHECK(narrow.value.empty());
}

TEST_CASE("ASCII text converts in both directions") {
    const Utf16ConversionResult wide = to_utf16("Hello, FolderPilot!");
    REQUIRE(wide.ok);
    CHECK(wide.value == L"Hello, FolderPilot!");

    const Utf8ConversionResult narrow = to_utf8(L"Hello, FolderPilot!");
    REQUIRE(narrow.ok);
    CHECK(narrow.value == "Hello, FolderPilot!");
}

TEST_CASE("explicit lengths are honored (no truncation, no overrun)") {
    // A view explicitly limited to the first 3 code units of "Hello".
    const Utf16ConversionResult truncated = to_utf16(std::string_view{"Hello", 3});
    REQUIRE(truncated.ok);
    CHECK(truncated.value == L"Hel");

    // Embedded NUL bytes must survive: length comes from the view, not from
    // a terminating null.
    const std::string with_nul = std::string{"ab"} + '\0' + "cd";
    REQUIRE(with_nul.size() == 5);
    const Utf16ConversionResult wide = to_utf16(with_nul);
    REQUIRE(wide.ok);
    CHECK(wide.value.size() == 5);
    check_round_trip(with_nul);
}

TEST_CASE("Indian-language text (Devanagari) converts and round-trips") {
    check_round_trip("\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87"); // नमस्ते
}

TEST_CASE("CJK text converts and round-trips") {
    check_round_trip("\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\xE6\x95\xB4\xE7\x90\x86"); // 文件夹整理
}

TEST_CASE("emoji (surrogate pairs) convert with correct UTF-16 code units") {
    const Utf16ConversionResult wide = to_utf16("\xF0\x9F\x9A\x80"); // rocket U+1F680
    REQUIRE(wide.ok);
    REQUIRE(wide.value.size() == 2);
    CHECK(wide.value[0] == L'\xD83D'); // high surrogate
    CHECK(wide.value[1] == L'\xDE80'); // low surrogate

    // The surrogate pair converts back to the exact UTF-8 bytes.
    const Utf8ConversionResult narrow = to_utf8(std::wstring{L'\xD83D', L'\xDE80'});
    REQUIRE(narrow.ok);
    CHECK(narrow.value == "\xF0\x9F\x9A\x80");
}

TEST_CASE("combining characters round-trip without normalization") {
    // "cafe" + U+0301 COMBINING ACUTE ACCENT: deliberately NOT the
    // precomposed é - conversion must be byte-preserving, never normalizing.
    const std::string combining = "cafe" "\xCC\x81";
    check_round_trip(combining);
}

TEST_CASE("multiple scripts round-trip") {
    check_round_trip("résumé.pdf");          // Latin-1 supplement
    check_round_trip("\xD1\x84\xD0\xBE\xD1\x82\xD0\xBE.jpg"); // фото.jpg (Cyrillic)
    check_round_trip("FolderPilot 文件夹 1.0");
}

TEST_CASE("invalid UTF-8 is rejected, never silently repaired") {
    const char* invalid_inputs[] = {
        "\xFF",            // invalid byte
        "abc\xC3",         // truncated two-byte sequence
        "\xE2\x82",        // truncated three-byte sequence
        "\xED\xA0\x80",    // UTF-8-encoded surrogate U+D800 (forbidden)
        "\xC0\x80",        // overlong encoding of NUL
    };
    for (const std::string_view bad : invalid_inputs) {
        const Utf16ConversionResult result = to_utf16(bad);
        CHECK_FALSE(result.ok);
        CHECK(!result.error.empty());
        CHECK(result.value.empty());
    }
}

TEST_CASE("invalid UTF-16 is rejected, never silently repaired") {
    const std::wstring invalid_inputs[] = {
        std::wstring{L'\xD800'},                    // lone high surrogate
        std::wstring{L'\xD83D'},                    // unpaired high surrogate
        std::wstring{L'a', L'\xDC00', L'b'},        // lone low surrogate mid-string
    };
    for (const std::wstring& bad : invalid_inputs) {
        const Utf8ConversionResult result = to_utf8(bad);
        CHECK_FALSE(result.ok);
        CHECK(!result.error.empty());
        CHECK(result.value.empty());
    }
}

TEST_CASE("valid surrogate pair embedded in text round-trips") {
    check_round_trip("before \xF0\x9F\x9A\x80 after");
}
