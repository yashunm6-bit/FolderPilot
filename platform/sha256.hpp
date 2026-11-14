// FolderPilot - SHA-256 via the Windows CNG provider (BCrypt). G1 probe.
//
// Phase 7 will use this to integrity-check journal records. The header
// exposes a portable interface (no Windows types): Win32/BCrypt lives only
// in sha256.cpp, per the layering rule that Win32 exists only inside
// platform/. Linked with the bcrypt import library (-lbcrypt).
//
// G1 status: CLOSED (2026-11-14) - hosted run 43598561201 (commit dea0dc6)
// verified the bcrypt linkage and all four NIST FIPS 180-4 vectors on
// windows-latest in Debug and Release (docs/PHASE7-DESIGN.md section 16).
// This wrapper is the production integrity primitive for the Phase 7 move
// journal (lib/folderpilot/journal.cpp hashes every record through it).

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace folderpilot::platform {

// SHA-256 digest size in bytes (FIPS 180-4).
inline constexpr std::size_t sha256_digest_bytes = 32;

struct Sha256Result {
    bool ok = false;
    std::array<unsigned char, sha256_digest_bytes> digest{};
    std::string error;              // empty when ok
    unsigned long native_code = 0;  // NTSTATUS of the failing BCrypt call
};

// Computes the SHA-256 digest of data in one shot. Empty input is valid and
// yields the FIPS 180-4 empty-string vector. No exceptions: failures are
// returned in the result.
[[nodiscard]] Sha256Result sha256(std::span<const unsigned char> data);

} // namespace folderpilot::platform
