// FolderPilot - G1 probe: BCrypt SHA-256 against NIST FIPS 180-4 vectors.
//
// These tests are the G1 evidence. They can only pass if (a) the bcrypt
// import library linked, (b) the CNG provider produced correct digests.
// They execute on hosted CI (windows-latest), which runs the freshly built
// binary that Smart App Control blocks locally.
//
// Vectors (FIPS 180-4 / NIST examples):
//   empty string, "abc", the 448-bit message, and the 896-bit multi-block
//   message.

#include <doctest.h>

#include "sha256.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace {

std::string to_hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(digits[bytes[i] >> 4]);
        hex.push_back(digits[bytes[i] & 0x0F]);
    }
    return hex;
}

struct KnownAnswerVector {
    const char* label;
    std::string message;
    const char* expected_hex;
};

const KnownAnswerVector nist_vectors[] = {
    {"empty string", "",
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "abc",
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"448-bit message", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"896-bit multi-block message",
     "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
     "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
     "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
};

} // namespace

TEST_CASE("SHA-256 BCrypt wrapper matches NIST FIPS 180-4 vectors (G1)") {
    for (const auto& vector : nist_vectors) {
        const std::vector<unsigned char> data(vector.message.begin(),
                                              vector.message.end());
        const auto result = folderpilot::platform::sha256(
            std::span<const unsigned char>(data));
        REQUIRE_MESSAGE(result.ok, vector.label,
                        ": hashing failed: ", result.error);
        CHECK_MESSAGE(to_hex(result.digest.data(), result.digest.size()) ==
                          vector.expected_hex,
                      vector.label, ": digest mismatch");
    }
}

TEST_CASE("SHA-256 BCrypt wrapper is deterministic (G1)") {
    const std::vector<unsigned char> data{'a', 'b', 'c'};
    const auto first = folderpilot::platform::sha256(
        std::span<const unsigned char>(data));
    REQUIRE(first.ok);
    const auto second = folderpilot::platform::sha256(
        std::span<const unsigned char>(data));
    REQUIRE(second.ok);
    CHECK(first.digest == second.digest);
}

TEST_CASE("SHA-256 empty input yields the FIPS empty-string vector (G1)") {
    const auto result = folderpilot::platform::sha256(
        std::span<const unsigned char>());
    REQUIRE_MESSAGE(result.ok, "empty-input hashing failed: ", result.error);
    CHECK(to_hex(result.digest.data(), result.digest.size()) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
